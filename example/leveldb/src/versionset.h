#pragma once
#include <map>
#include <set>
#include <vector>
#include <list>
#include <deque>
#include <assert.h>
#include "logwriter.h"
#include "tablecache.h"
#include "versionedit.h"
#include "option.h"
#include "dbformat.h"

class VersionSet;
class Version
{
public:
	Version(VersionSet *vset)
		:vset(vset),
		refs(0)
	{

	}

	VersionSet *vset;     // VersionSet to which this Version belongs

	// Lookup the value for key.  If found, store it in *val and
	// return OK.  Else return a non-OK status.  Fills *stats.
	// REQUIRES: lock is not held
	struct GetStats
	{
		std::shared_ptr<FileMetaData> seekFile;
		int seekFileLevel;
	};

	Status get(const ReadOptions &options, const LookupKey &key, std::string *val,
		GetStats *stats);
	// List of files per level
	std::vector<std::shared_ptr<FileMetaData>> files[kNumLevels];

	// Next file to compact based on seek stats.
	std::shared_ptr<FileMetaData> fileToCompact;
	int fileToCompactLevel;

	// Return the level at which we should place a new memtable compaction
	// result that covers the range [smallest_user_key,largest_user_key].
	int pickLevelForMemTableOutput(const std::string_view &smallestUserKey,
		const std::string_view &largestUserKey);

	void getOverlappingInputs(
		int level,
		const InternalKey *begin,         // nullptr means before all keys
		const InternalKey *end,           // nullptr means after all keys
		std::vector<std::shared_ptr<FileMetaData>> *inputs);
	
	void saveValue(const std::any &arg, const std::string_view &ikey, const std::string_view &v);
	// Returns true iff some file in the specified level overlaps
	// some part of [*smallest_user_key,*largest_user_key].
	// smallest_user_key==nullptr represents a key smaller than all the DB's keys.
	// largest_user_key==nullptr represents a key largest than all the DB's keys.

	bool overlapInLevel(int level, const std::string_view *smallestUserKey,
		const std::string_view *largestUserKey);

	// Adds "stats" into the current state.  Returns true if a new
	// compaction may need to be triggered, false otherwise.
	// REQUIRES: lock is held
	bool updateStats(const GetStats &stats);
	  
	// Level that should be compacted next and its compaction score.
	// Score < 1 means compaction is not strictly needed.  These fields
	// are initialized by Finalize().
	double compactionScore;
	int compactionLevel;
	int refs;

private:
	// No copying allowed
	Version(const Version&);
	void operator=(const Version&);
};


class Builder
{
public:
	Builder(VersionSet *vset, Version *base);
	~Builder();
	
	// Apply all of the edits in *edit to the current state.
	void apply(VersionEdit *edit);

	// Save the current state in *v.
	void saveTo(Version *v);

	void maybeAddFile(Version *v, int level, const std::shared_ptr<FileMetaData> &f);
private:
	// Helper to sort by v->files_[file_number].smallest
	struct BySmallestKey
	{
		const InternalKeyComparator *internalComparator;
		bool operator()(const std::shared_ptr<FileMetaData> &f1,
			const std::shared_ptr<FileMetaData> &f2) const
		{
			int r = internalComparator->compare(f1->smallest, f2->smallest);
			if (r != 0)
			{
				return (r < 0);
			}
			else
			{
				// Break ties by file number
				return (f1->number < f2->number);
			}
		}
	};

	typedef std::set<std::shared_ptr<FileMetaData>, BySmallestKey> FileSet;
	struct LevelState
	{
		std::set<uint64_t> deletedFiles;
		std::shared_ptr<FileSet> addedFiles;
	};

	VersionSet *vset;
	Version *base;
	LevelState levels[kNumLevels];
};

class VersionSet
{
public:
	VersionSet(const std::string &dbname, const Options &options,
			const std::shared_ptr<TableCache> &tableCache, const InternalKeyComparator *cmp);
	~VersionSet();

	uint64_t getLastSequence() const { return lastSequence; }
	void setLastSequence(uint64_t s)
	{
		assert(s >= lastSequence);
		lastSequence = s;
	}

	// Returns true iff some level needs a compaction.
	bool needsCompaction() const
	{
		assert(!versions.empty());
		auto v = versions.front();
		return (v->compactionScore >= 1) || (v->fileToCompact != nullptr);
	}

	// Arrange to reuse "file_number" unless a newer file number has
	// already been allocated.
	// REQUIRES: "file_number" was returned by a call to NewFileNumber().
	void reuseFileNumber(uint64_t fileNumber)
	{
		if (nextFileNumber == fileNumber + 1)
		{
			nextFileNumber = fileNumber;
		}
	}

	std::shared_ptr<Version> current() const;
	// Apply *edit to the current version to form a new descriptor that
	// is both saved to persistent state and installed as the new
	// current version.  Will release *mu while actually writing to the file.
	// REQUIRES: *mu is held on entry.
	// REQUIRES: no other thread concurrently calls LogAndApply()
	Status logAndApply(VersionEdit *edit);

	void finalize(Version* v);
	// Add all files listed in any live version to *live.
	// May also mutate some internal state.
	void addLiveFiles(std::set<uint64_t> *live);

	uint64_t getLogNumber() { return logNumber; }
	uint64_t getPrevLogNumber() { return prevLogNumber; }

	const InternalKeyComparator icmp;

	uint64_t newFileNumber() { return nextFileNumber++; }

	Status recover(bool *manifest);
	void markFileNumberUsed(uint64_t number);

	uint64_t getManifestFileNumber() { return manifestFileNumber; }

	void appendVersion(const std::shared_ptr<Version> &v);
	bool reuseManifest(const std::string &dscname, const std::string &dscbase);
	int numLevelFiles(int level) const;

	std::shared_ptr<TableCache> getTableCache() { return tableCache; }
	// Per-level key at which the next compaction at that level should start.
	// Either an empty string, or a valid InternalKey.
	std::string compactPointer[kNumLevels];
	const std::string dbname;
	const Options options;
	uint64_t nextFileNumber;
	uint64_t manifestFileNumber;
	uint64_t lastSequence;
	uint64_t logNumber;
	uint64_t prevLogNumber;  // 0 or backing store for memtable being compacted
	std::list<std::shared_ptr<Version>> versions;
	std::shared_ptr<LogWriter> descriptorLog;
	std::shared_ptr<WritableFile> descriptorFile;
	std::shared_ptr<TableCache> tableCache;
};


int findFile(const InternalKeyComparator &icmp,
	const std::vector<std::shared_ptr<FileMetaData>> &files,
	const std::string_view &key);


bool someFileOverlapsRange(const InternalKeyComparator &icmp, bool disjointSortedFiles,
	const std::vector<std::shared_ptr<FileMetaData>> &files,
	const std::string_view *smallestUserKey,
	const std::string_view *largestUserKey);


// A Compaction encapsulates information about a compaction.
class Compaction 
{
public:
	Compaction();
};

