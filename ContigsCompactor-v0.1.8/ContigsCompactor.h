//
//  ContigsCompactor.h
//  
//
//  Created by Yufeng Wu on 2/18/15.
//  Compare contigs and find duplicates;
//  Goal: produce a set of contigs that are not as redundent

//  Edit: convert to multi-thread version by Chong Chu on 05/18/15
//	Change the QuickChecker part to Multi-thread using hashing
//  The pairwise comparison part is changed to multi-thread

#ifndef ____ContigsCompactor__
#define ____ContigsCompactor__

#include <iostream>
#include <vector>
#include <cstdlib>
#include "fastaMultiSeqs.h"
#include "GraphUtils.h"
#include "KmerUtils.h"
#include <map>
#include <pthread.h>

using namespace std;

// ******************************************************************
// Testing

void TestGraph( );


// ******************************************************************
// merge info: how should we merge 

class ContigsCompactorAction
{
public:
    ContigsCompactorAction() : posEnd(0), szSeq1(0), szSeq2(0) {}
    void SetMergedString( const string &str) { strCompact = str; }
    string GetMerged() const { return strCompact; }
    int GetPosEndSeq1() const {return posEnd;}
    void SetPosEndSeq1(int pos) { posEnd = pos; }
    int GetMergedLen() const { return strCompact.length(); }
    void SetOrigSeqLen(int s1, int s2) { szSeq1 = s1; szSeq2 = s2; }
    int GetOverlapSize() const { return szSeq1+szSeq2-GetMergedLen(); }
    void SetAlnSeqs( const char * pstr1, const char *pstr2) { alnStr1 = pstr1; alnStr2 = pstr2; }
    void SetDPEnds(int v1, int v2) { posRowEnd = v1; posColEnd = v2; }
    void SetMergedStringConcat();
    bool IsContainment() const;
    
private:
    string strCompact;
    int posEnd;
    int szSeq1;
    int szSeq2;
    string alnStr1;
    string alnStr2;
    int posRowEnd;
    int posColEnd;
};

// ******************************************************************
// Quick checker of two sequences can have a chance to merge
class QuickCheckerContigsMatch
{
public:
    QuickCheckerContigsMatch();
    QuickCheckerContigsMatch(const QuickCheckerContigsMatch &rhs);
    QuickCheckerContigsMatch( FastaSequence *pRepeatSeq, int kmerLen );
    bool IsMatchFeasible( FastaSequence *pScaffoldSeq ) const;
    bool IsMatchFeasibleV2( FastaSequence *pScaffoldSeq ) const;
    
private:
    //
    void Init();
    void GetKmersList( FastaSequence *pSeq, int posStart, int lenSeq, vector<KmerTypeShort> &listKmers ) const;
    bool AreKmersMatch( const vector<KmerTypeShort> &listKmers, double minRatioKmerMatch ) const;
    
    FastaSequence *pRepeatSeq;
    int kmerLen;
    map<KmerTypeShort,int> mapKmerFreqInRepeat;
};


// *****************************************************************
// Quick hash checker with multi-thread
struct KmerNode{
	int index;
	int pos;
};
typedef std::vector<KmerNode> VNode;


class MultiThreadHashChecker
{
public:
	MultiThreadHashChecker();
	MultiThreadHashChecker(vector< FastaSequence *>& vfs,int kmerLen, int nThreads);
	void runMultiHash();
	void gnrtHashCheckTab();
	void releaseHashCheckTab();
	void outputHashTab(int minSupport);

private:
	static void *threadHashContigV1(void *ptr);

public:
	static vector< FastaSequence *> vfs;
	static int** ptabOverlap;

private://declaration of static variables
	static int K;
	static int SEED;
	static std::map<uint32_t, VNode> mapKmersV1;
	static pthread_mutex_t mutex;

private:
	int contigSize;
	int T;
};


// ******************************************************************
// Compact contigs 

class AbstractGraphNode;

class ContigsCompactor
{
public:
    ContigsCompactor();
    void Compact(  MultiFastqSeqs &listRepeats );
    void CompactVer2(  MultiFastqSeqs &listRepeats );
    void CompactVer3(  MultiFastqSeqs &listRepeats, const char* fileScaffoldInfo, int miniSupportPairCutOff);
    void SetVerbose(bool f) { fVerbose = f; }
    void SetFractionLossScore(double f) { fractionLossScore = f; }
    void SetMinOverlap(double f) { fracMinOverlap = f; }
    void SetLenContigOut(int f) { lenContigOut = f; }
    void SetMinOverlapLen(double len) { minOverlapLen = len; }
	void SetMinOverlapLenWithScaffold(double len){minOverlapLenWithScaffold=len;}
    void SetQuickCheckKmerLen(int len) { kmerLenQuick = len; }
    void SetMismatchScore(double s) { scoreMismatch = s; }
    void SetIndelScore(double s) { scoreIndel = s; }
    MultiFastqSeqs &GetNewSeqs() { return setNewSeqsOnly; }
    void OutputContigsInfo(const char *fileOut);
    void OutputContigsInfoVer2(const char *fileOut);
    void SetMaxContigPathLen(int len) { maxContigPathLen = len; }
    void SetMaxCountPerContigInPaths(int cnt) { maxCountPerContigInPaths = cnt; }
	void SetNumOfThreads(int cnt){numOfThreads=cnt;}
	void SetMinSupportKmers(int cnt){minSupportKmers=cnt;}

private:
    int Evaluate(FastaSequence *pSeq1, FastaSequence *pSeq2, ContigsCompactorAction &resCompact, bool fRelax = false);
    int IsScoreSignificant( int scoreMax, int szSeq1, int szSeq2, int rowStart, int colStart  ) const;
    string FormMergedSeqFromPath( const vector<AbstractGraphNode *> &listPathNodes  );
    void RemoveDupRevCompPaths( set<vector<AbstractGraphNode *> > &setPaths, map<AbstractGraphNode *, AbstractGraphNode *> &mapListRevCompNodes );
    
    double fractionLossScore;
    double fracMinOverlap;
    double minOverlapLen;
	double minOverlapLenWithScaffold;
	int numOfThreads;
	int minSupportKmers;
    int kmerLenQuick;
    int lenContigOut;
    double scoreMismatch;
    double scoreIndel;
    bool fVerbose;
    int maxContigPathLen;
    int maxCountPerContigInPaths;
    MultiFastqSeqs setNewSeqsOnly;
    map<FastaSequence *, vector< pair<FastaSequence *, int> > > mapNewCOntigSrcCtgInfo;
    map<string, string> mapNewContigNameToPath;

private:
	static map< FastaSequence *, GraphNodeRefExt * > mapContigToGraphNode;
	static pthread_mutex_t merge_mutex;
	static vector< std::pair<int,int> > vMergePairs;
	static vector< std::pair<int,int> > tempMergeInfo;
	static int numOfContigs;
	static int minNumKmerSupport;

private:
	void runMultiThreadMerge(int minNumKmerSupport, int T);
	static void *threadMergeContig(void *ptr);
	
};

typedef struct
{
	std::pair< std::pair<int,int>, std::pair<int,int> > prang;
	ContigsCompactor* pthc;
	bool isVerbose;
}ParNode;

#endif /* defined(____ContigsCompactor__) */