//
//  ContigsCompactor.cpp
//  
//
//  Created by Yufeng Wu on 2/18/15.
//  
//  Edit: convert to multi-thread version by Chong Chu on 06/18/15
//	Change the QuickChecker part to Multi-thread.
//  The pairwise comparison part is changed to multi-thread.
//
//	Fix one bug on DP part (04/12/16, C.C.): when check whether one is contained in another,
//                          some are wrongly considered as contained which are overlap
//
//  Edit: In this version, we are going to allow some mismatch at one end of the two sequences to be merged. (By Chong Chu on 04/29/16)

#include <algorithm>
#include <iostream>
#include <fstream>
#include <set>
#include <pthread.h>
#include <ctime>
#include <cstring>
#include "GenSeqsUtils.h"
#include "Utils-basic.h"
#include "ContigsCompactor.h"
#include "GraphUtils.h"
#include "ScaffoldUtils.h"
#include "MurmurHash3.h"


const int MAX_NEG_SCORE = -1000000000;
const int QUICK_CHECK_KMER_LEN = 10;
const int MAX_CONTIGS_IN_PATH = 50;
const int MAX_CONTIG_IN_PATH_COUNT = 20;
//const int QUICK_CHECK_KMER_LEN = 3;
const int OVERLAP_SMALLER_MINLENSCAFFOLD=0;
const int OVERLAP_IN_RANGE=1;
const int OVERLAP_LARGER_MINLEN=2;


double ContigsCompactor::fractionLossScore = 0.05;
double ContigsCompactor::fracMinOverlap = 0.01;
double ContigsCompactor::minOverlapLen= 10000;
double ContigsCompactor::maxOverlapClipLen = 0;
double ContigsCompactor::minOverlapLenWithScaffold= QUICK_CHECK_KMER_LEN;

double ContigsCompactor::scoreMismatch= -1.0;
double ContigsCompactor::scoreIndel= -1.0;
bool ContigsCompactor::fVerbose = true;

//definition of the static variables in multi-thread quick check
int MultiThreadQuickChecker::numOfContigs;
vector< QuickCheckerContigsMatch>  MultiThreadQuickChecker::listQuickCheckers;

//definition of the static variables in multi-thread ContigsCompactor Contigs
map< FastaSequence *, GraphNodeRefExt * > ContigsCompactor::mapContigToGraphNode;
vector< std::pair<int,int> > ContigsCompactor::vMergePairs;
vector< std::pair<int,int> > ContigsCompactor::tempMergeInfo;
vector<ConnectionInfo> ContigsCompactor::vConnectInfo;
long ContigsCompactor::vistPos;

int ContigsCompactor::numOfContigs;
int ContigsCompactor::minNumKmerSupport;

std::vector< FastaSequence *> ContigsCompactor::vfs;

static pthread_mutex_t merge_mutex= PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t merge_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t check_mutex= PTHREAD_MUTEX_INITIALIZER;
// ******************************************************************
// Testing

void TestGraph()
{
    // create a graph and check SCC
    AbstractGraph graphSimple;
    AbstractGraphNode *pnode1 = new AbstractGraphNode("node1");
    AbstractGraphNode *pnode2 = new AbstractGraphNode("node2");
    AbstractGraphNode *pnode3 = new AbstractGraphNode("node3");
    AbstractGraphNode *pnode4 = new AbstractGraphNode("node4");
    pnode1->AddNgbrNodeBasic( pnode2 );
    pnode1->AddNgbrNodeBasic( pnode3);
    pnode2->AddNgbrNodeBasic( pnode3 );
    pnode3->AddNgbrNodeBasic( pnode1);
    pnode2->AddNgbrNodeBasic( pnode4 );
    graphSimple.AddNode(pnode1);
    graphSimple.AddNode(pnode2);
    graphSimple.AddNode(pnode3);
    graphSimple.AddNode(pnode4);
    
    vector< set<AbstractGraphNode *> > setSCCs;
    graphSimple.SCC( setSCCs );
    
    cout << "Number of SCCs: " << setSCCs.size() << endl;
    for(int i=0; i<(int)setSCCs.size(); ++i)
    {
        cout << "*** Find one component: \n";
        for( set<AbstractGraphNode *> :: iterator it = setSCCs[i].begin(); it != setSCCs[i].end(); ++it)
        {
            (*it)->Dump();
        }
    }

}

// ******************************************************************
// merge info: how should we merge
void ContigsCompactorAction :: SetMergedStringConcat()
{
    // set the merged string to be something really simple
    // if one contain another, merged string is simply the containing one
    // if overlap, then just take prefix/suffix of the two strings
    
//cout << "SetMergedStringConcat: posRowEnd: " << posRowEnd << ", posColEnd: " << posColEnd << ", aln1str: " << alnStr1.length() << ", aln2str: " << alnStr2.length() << endl;

	if(bcontained && (posRowEnd+nclip) == (int)alnStr1.length() && alnStr1.length() <  alnStr2.length()  )
    {
//cout << "Contained: 2 contain 1\n";
        // alnStr1 is contained
        strCompact = alnStr2;
    }
    else  if(bcontained && (posColEnd+nclip) == (int)alnStr2.length() && alnStr2.length() < alnStr1.length()  )
    {
//cout << "Contained: 1 contain 2\n";
        // alnStr1 is contained
        strCompact = alnStr1;
    }
    else
    {
        // this should be the case of prefix/suffix overlap
        if( (posRowEnd+nclip) == (int)alnStr1.length() )
        {
//cout << "Prefix/suffix: 1 follow by 2\n";
            //
            /*if( (int)alnStr1.length()<posColEnd )
            {
                cout << "WARNING: wrong0\n";
            }*/
			strCompact = alnStr1.substr(0,alnStr1.length()-nclip) + alnStr2.substr(posColEnd, (int)alnStr2.length()-posColEnd);
        }
        else
        {
//cout << "Prefix/suffix: 2 follow by 1\n";
            /*if( (int)alnStr2.length() < posRowEnd )
            {
                cout << "WARNING: wrong1\n";
            }*/
            strCompact = alnStr2.substr(0, alnStr2.length() - nclip) + alnStr1.substr(posRowEnd, (int)alnStr1.length()-posRowEnd );   
        }
    }
    
//cout << "SetMergedStringConcat: alnste1 = " << alnStr1 << ", alnstr2 = " << alnStr2 << ", merged string: " << strCompact << endl;
}

bool ContigsCompactorAction :: IsContainment() const
{
    //
    return bcontained && (( (posRowEnd+nclip) == (int)alnStr1.length() && (int)alnStr1.length() <  posColEnd ) || ( (posColEnd+nclip) == (int)alnStr2.length() && (int)alnStr2.length() < posRowEnd  ));
}

// ******************************************************************
// Compact contigs

//ContigsCompactor :: ContigsCompactor() : fractionLossScore(0.05), fracMinOverlap(0.5),minOverlapLen(10000), kmerLenQuick(QUICK_CHECK_KMER_LEN), scoreMismatch(-1.0), scoreIndel(-1.0), fVerbose(true), maxContigPathLen(MAX_CONTIGS_IN_PATH), maxCountPerContigInPaths(MAX_CONTIG_IN_PATH_COUNT)
//{
//}

ContigsCompactor::ContigsCompactor() :  kmerLenQuick(QUICK_CHECK_KMER_LEN), maxContigPathLen(MAX_CONTIGS_IN_PATH), maxCountPerContigInPaths(MAX_CONTIG_IN_PATH_COUNT)
{
}

//void ContigsCompactor :: Compact( MultiFastqSeqs &listContigs )
//{
//    // just do one by one
//    bool fCont = true;
//    while(fCont == true)
//    {
//        // create a list of quckchecker
//        // construct all the quick checkers
//        vector< vector< QuickCheckerContigsMatch> > listQuickCheckers( listContigs.GetNumOfSeqs() );
//        for(int j=0; j<listContigs.GetNumOfSeqs(); ++j)
//        {
//            // now perform quick check
//            QuickCheckerContigsMatch qchecker(listContigs.GetSeq(j), kmerLenQuick);
//            
//            // also check the reverse complement
//            FastaSequence seqRevComp( *listContigs.GetSeq(j) );
//            seqRevComp.RevsereComplement();
//            QuickCheckerContigsMatch qcheckerRev( &seqRevComp, kmerLenQuick);
//            listQuickCheckers[j].push_back(qchecker);
//            listQuickCheckers[j].push_back(qcheckerRev);
//        }
//
//        
//        
//        fCont = false;
//        for(int i=0; i<listContigs.GetNumOfSeqs(); ++i)
//        {
//            // also check the reverse complement
//            FastaSequence seqRevComp( *listContigs.GetSeq(i) );
//            seqRevComp.RevsereComplement();
//            
//            // now check each of the following ones
//            for(int j=i+1; j<listContigs.GetNumOfSeqs(); ++j)
//            {
//                ContigsCompactorAction ccAct;
//                bool fMatch = false;
//                if( listQuickCheckers[i][0].IsMatchFeasible( listContigs.GetSeq(j) ) == true )
//                {
//                    fMatch = Evaluate( listContigs.GetSeq(i), listContigs.GetSeq(j), ccAct);
//                }
//                if( fMatch == false)
//                {
//                    //
//                    if( listQuickCheckers[i][1].IsMatchFeasible( listContigs.GetSeq(j) ) == true  )
//                    {
//                        fMatch = Evaluate(&seqRevComp, listContigs.GetSeq(j), ccAct);
//                    }
//                }
//                
//                if( fMatch == true)
//                {
//                    if( fVerbose == true )
//                    {
//                        //cout << "One contigs merged: number of contigs: " << listContigs.GetNumOfSeqs() << endl;
//                        //cout << "Contigs: ";
//                        //listContigs.GetSeq(i)->printFasta(cout);
//                        //cout << " can be merged with contig: ";
//                        //listContigs.GetSeq(j)->printFasta(cout);
//                        //cout << "RESULT: " << ccAct.GetMerged() << endl;
//                    }
//                    // erase the two original sequence and add the new one 
//                    FastaSequence *pseqOrig1 =  listContigs.GetSeq(i);
//                    FastaSequence *pseqOrig2 =  listContigs.GetSeq(j);
//
//                    FastaSequence *pSeqNew = new FastaSequence;
//                    // set to the string name that is longer
//                    if( pseqOrig1->size() >= pseqOrig2->size() )
//                    {
//                        pSeqNew->SetName( pseqOrig1->name() );
//                    }
//                    else
//                    {
//                        pSeqNew->SetName(pseqOrig2->name()  );
//                    }
//                    
//                    listContigs.EraseSeq( pseqOrig1 );
//                    listContigs.EraseSeq( pseqOrig2 );
//
//                    pSeqNew->AppendSeq( ccAct.GetMerged() );
//                    listContigs.Append( pSeqNew );
//                    
//                    // also add to the set of new contigs
//                    FastaSequence *pSeqNewCopy = new FastaSequence( *pSeqNew );
//                    setNewSeqsOnly.Append( pSeqNewCopy );
//cout << "HAVING ONE NEW CONTIGS\n";
////exit(1);
//                    fCont = true;
//                    break;
//                }
//
//            }
//            if(fCont )
//            {
//                break;
//            }
//
//        }
//
//    }
//}

//static void ReverseContigInfoList( vector< pair<FastaSequence *,int>  > &listContigsInfo )
//{
//    //
//    vector< pair<FastaSequence *,int>  > res;
//    for(int i=(int)listContigsInfo.size()-1; i>=0; --i)
//    {
//        pair<FastaSequence *,int> pp( listContigsInfo[i].first, -1*listContigsInfo[i].second );
//        res.push_back(pp);
//    }
//    listContigsInfo = res;
//}


//void ContigsCompactor :: CompactVer2( MultiFastqSeqs &listContigs )
//{
//    // now try some speedup
//    //bool fCont = true;
//    
//    set<pair<FastaSequence *, FastaSequence *> > setPairsContigsDone;
//    //set<string> setContigsMerged;
//    // recall what have been merged
//    map<string, set<FastaSequence *> > mapNewContigSrcCtgPtrs;
//    // info: use an integer (1 or -1) to indicate whether to reverse complement or not
//    //map<string, vector< pair<FastaSequence *, int> > > mapNewCOntigSrcCtgInfo;
//    mapNewCOntigSrcCtgInfo.clear();
//    
//    while(true)
//    {
////cout << "Number of sequences: " << listContigs.GetNumOfSeqs() << endl;
//        // create a list of quckchecker
//        // construct all the quick checkers
//        vector< vector< QuickCheckerContigsMatch> > listQuickCheckers( listContigs.GetNumOfSeqs() );
//        for(int j=0; j<listContigs.GetNumOfSeqs(); ++j)
//        {
//            // now perform quick check
//            QuickCheckerContigsMatch qchecker(listContigs.GetSeq(j), kmerLenQuick);
//            
//            // also check the reverse complement
//            FastaSequence seqRevComp( *listContigs.GetSeq(j) );
//            seqRevComp.RevsereComplement();
//            QuickCheckerContigsMatch qcheckerRev( &seqRevComp, kmerLenQuick);
//            listQuickCheckers[j].push_back(qchecker);
//            listQuickCheckers[j].push_back(qcheckerRev);
//        }
//        
//        // store the list of already done contigs
//        //set<FastaSequence *> setInvolvedSeqPtrs;
//        set<pair<string,string> > setNewContigStrs;
//        bool fNewThing = false;
//        
//        //fCont = false;
//        vector< pair<FastaSequence *, int> > vecSeqsCombo;
//        
//        for(int i=0; i<listContigs.GetNumOfSeqs(); ++i)
//        {
////cout << "Doing i = " << i << endl;
//            // skip if this contig has been processed before
//            //if( setInvolvedSeqPtrs.find( listContigs.GetSeq(i) ) != setInvolvedSeqPtrs.end() )
//            //{
//            //    continue;
//            //}
//            
//            // what src contigs have been involved?
//            set<FastaSequence *> setSeqs1;
//            string strContig1 = listContigs.GetSeq(i)->c_str();
//            if( mapNewContigSrcCtgPtrs.find( strContig1 ) != mapNewContigSrcCtgPtrs.end() )
//            {
//                setSeqs1 = mapNewContigSrcCtgPtrs[ strContig1 ];
//            }
//            setSeqs1.insert( listContigs.GetSeq(i) );
//            
//            // also prepare the contig info
//            vector< pair<FastaSequence *, int> > vecSeqs1;
//            if( mapNewCOntigSrcCtgInfo.find(listContigs.GetSeq(i)) != mapNewCOntigSrcCtgInfo.end() )
//            {
//                vecSeqs1 = mapNewCOntigSrcCtgInfo[listContigs.GetSeq(i)];
//            }
//            else
//            {
//                pair<FastaSequence *,int> pp( listContigs.GetSeq(i), 1 );
//                vecSeqs1.push_back(pp);
//            }
//            vector< pair<FastaSequence *,int>  > vecSeqs1Rev = vecSeqs1;
//            ReverseContigInfoList( vecSeqs1Rev );
//            
//            // also check the reverse complement
//            FastaSequence seqRevComp( *listContigs.GetSeq(i) );
//            seqRevComp.RevsereComplement();
//            
//            // now check each of the following ones
//            for(int j=i+1; j<listContigs.GetNumOfSeqs(); ++j)
//            {
////cout << "Doing j = " << j << endl;
//                // skip if this contig has been processed before
//                //if( setInvolvedSeqPtrs.find( listContigs.GetSeq(j) ) != setInvolvedSeqPtrs.end() )
//                //{
//                //    continue;
//                //}
//                
//                
//                // deal w/ processed ones
//                FastaSequence *pseqOrig1 =  listContigs.GetSeq(i);
//                FastaSequence *pseqOrig2 =  listContigs.GetSeq(j);
//                // remember the pair of contigs done
//                pair<FastaSequence *, FastaSequence *> pp1( pseqOrig1, pseqOrig2 );
//                pair<FastaSequence *, FastaSequence *> pp2( pseqOrig2, pseqOrig1 );
//                bool ff = setPairsContigsDone.find(pp1) != setPairsContigsDone.end() || setPairsContigsDone.find(pp2) != setPairsContigsDone.end() ;
//                setPairsContigsDone.insert(pp1);
//                setPairsContigsDone.insert(pp2);
//                if( ff == true )
//                {
//                    // this pair has been done
//                    //cout << "This pair has been done.\n";
//                    continue;
//                }
//              
//                // what src contigs have been involved?
//                set<FastaSequence *> setSeqs2;
//                string strContig2 = listContigs.GetSeq(j)->c_str();
//                if( mapNewContigSrcCtgPtrs.find( strContig2 ) != mapNewContigSrcCtgPtrs.end() )
//                {
//                    setSeqs2 = mapNewContigSrcCtgPtrs[ strContig2 ];
//                }
//                setSeqs2.insert( listContigs.GetSeq(j) );
//                
//                // also prepare the contig info
//                vector< pair<FastaSequence *, int> > vecSeqs2;
//                if( mapNewCOntigSrcCtgInfo.find(listContigs.GetSeq(j) ) != mapNewCOntigSrcCtgInfo.end() )
//                {
//                    vecSeqs2 = mapNewCOntigSrcCtgInfo[listContigs.GetSeq(j) ];
//                }
//                else
//                {
//                    pair<FastaSequence *,int> pp( listContigs.GetSeq(j), 1 );
//                    vecSeqs2.push_back(pp);
//                }
//                
//                // if there are overlap, forbid the merging. Purpose: avoid infinite loop
//                bool fCont = true;
//                set<FastaSequence *> setComboPtrs = setSeqs2;
//                for( set<FastaSequence *> :: iterator it1 = setSeqs1.begin(); it1 != setSeqs1.end(); ++it1 )
//                {
//                    //
//                    if( setSeqs2.find(*it1) != setSeqs2.end() )
//                    {
//                        // there are overlap
//                        fCont = false;
//                        break;
//                    }
//                    setComboPtrs.insert( *it1 );
//                }
//                if( fCont == false )
//                {
//                    continue;
//                }
//                
//                
//                ContigsCompactorAction ccAct;
//                bool fMatch = false;
//                if( listQuickCheckers[i][0].IsMatchFeasible( listContigs.GetSeq(j) ) == true )
//                {
//                    fMatch = Evaluate( listContigs.GetSeq(i), listContigs.GetSeq(j), ccAct);
//                    
//                    if( fMatch == true)
//                    {
//                        //
//                        if( ccAct.GetPosEndSeq1() < listContigs.GetSeq(i)->size() )
//                        {
//                            // second go first
//                            vecSeqsCombo = vecSeqs2;
//                            std::copy (vecSeqs1.begin(), vecSeqs1.end(), std::back_inserter(vecSeqsCombo));
//                        }
//                        else
//                        {
//                            // first go first
//                            vecSeqsCombo = vecSeqs1;
//                            std::copy (vecSeqs2.begin(), vecSeqs2.end(), std::back_inserter(vecSeqsCombo));
//                        }
//                    }
//                }
//                if( fMatch == false)
//                {
//                    //
//                    if( listQuickCheckers[i][1].IsMatchFeasible( listContigs.GetSeq(j) ) == true  )
//                    {
//                        fMatch = Evaluate(&seqRevComp, listContigs.GetSeq(j), ccAct);
//                        
//                        if( fMatch == true)
//                        {
//                            //
//                            if( ccAct.GetPosEndSeq1() < listContigs.GetSeq(i)->size() )
//                            {
//                                // second go first then followed by a reversed copy
//                                vecSeqsCombo = vecSeqs2;
//                                std::copy (vecSeqs1Rev.begin(), vecSeqs1Rev.end(), std::back_inserter(vecSeqsCombo));
//                            }
//                            else
//                            {
//                                // first go first
//                                vecSeqsCombo = vecSeqs1Rev;
//                                std::copy (vecSeqs2.begin(), vecSeqs2.end(), std::back_inserter(vecSeqsCombo));
//                            }
//                        }
//                    }
//                }
//                
//                if( fMatch == true)
//                {
//                    
//                    if( fVerbose == true )
//                    {
//                        //cout << "One contigs merged: number of contigs: " << listContigs.GetNumOfSeqs() << endl;
//                        //cout << "Contigs: ";
//                        //listContigs.GetSeq(i)->printFasta(cout);
//                        //cout << " can be merged with contig: ";
//                        //listContigs.GetSeq(j)->printFasta(cout);
//                        //cout << "RESULT: " << ccAct.GetMerged() << endl;
//                    }
//                    // erase the two original sequence and add the new one
//                    //setInvolvedSeqPtrs.insert( pseqOrig1 );
//                    //setInvolvedSeqPtrs.insert( pseqOrig2 );
//                    
//                    // add to the result set
//                    string contigName;
//                    if( pseqOrig1->size() >= pseqOrig2->size() )
//                    {
//                        contigName =  pseqOrig1->name() ;
//                    }
//                    else
//                    {
//                        contigName = pseqOrig2->name() ;
//                    }
//                    
//                    pair<string,string> pp( ccAct.GetMerged(), contigName);
//                    
//                    //
//                    if(mapNewContigSrcCtgPtrs.find( ccAct.GetMerged() ) != mapNewContigSrcCtgPtrs.end()  )
//                    {
////cout << "This has been done before.\n";
//                        continue;
//                    }
//                    mapNewContigSrcCtgPtrs.insert(  map<string, set<FastaSequence *> > :: value_type( ccAct.GetMerged(), setComboPtrs )  );
////cout << "Conting merged: " << pp.first << ", name: " << pp.second << endl;
//                    
//                    
//                    setNewContigStrs.insert( pp );
//                    
//                    fNewThing = true;
//                    
////cout << "Adding something: " << endl;
//                    //FastaSequence *pSeqNew = new FastaSequence;
//                    // set to the string name that is longer
//                    //if( pseqOrig1->size() >= pseqOrig2->size() )
//                    //{
//                    //    pSeqNew->SetName( pseqOrig1->name() );
//                    //}
//                    //else
//                    //{
//                    //    pSeqNew->SetName(pseqOrig2->name()  );
//                    //}
//                    
//                    //listContigs.EraseSeq( pseqOrig1 );
//                    //listContigs.EraseSeq( pseqOrig2 );
//                    
//                    //pSeqNew->AppendSeq( ccAct.GetMerged() );
//                    //listContigs.Append( pSeqNew );
//                    
//                    //exit(1);
//                    //fCont = true;
//                    break;
//                }
//                
//            }
//            if( fNewThing == true)
//            {
//                break;
//            }
//            //if(fCont )
//            //{
//            //    break;
//            //}
//        }
//        
//        
////exit(1);
//        
//        // if nothing done, stop
//        if( fNewThing == false )
//        {
//            break;
//        }
//        //if( setInvolvedSeqPtrs.size() == 0 )
//        //{
//        //    //
//        //    break;
//        //}
//        if( setNewContigStrs.size() != 1 )
//        {
//            THROW("Fatal error3");
//        }
//        //for( set<FastaSequence *> :: iterator itg= setInvolvedSeqPtrs.begin(); itg != setInvolvedSeqPtrs.end(); ++itg  )
//        //{
//        //    // YW: don't erase old contigs
//        //    //listContigs.EraseSeq( *itg );
//        //}
//        for( set<pair<string,string> > :: iterator itg = setNewContigStrs.begin(); itg != setNewContigStrs.end(); ++itg )
//        {
//            // we must have some contig info
//            if(vecSeqsCombo.size() == 0)
//            {
//                cout << "FATAL ERROR\n";
//                exit(1);
//            }
//            
//            FastaSequence *pSeqNew = new FastaSequence;
//            static int indexNewContg = 1;
//            string cname = itg->second.c_str();
//            char buf[100];
//            sprintf(buf, "_%d", indexNewContg++);
//            cname += buf;
//            pSeqNew->SetName( cname.c_str() );
//            
//            // set to the string name that is longer
//            //pSeqNew->SetName( itg->second.c_str()  );
//            pSeqNew->AppendSeq( itg->first );
//            listContigs.Append( pSeqNew );
//            
//            // also add to the set of new contigs
//            FastaSequence *pSeqNewCopy = new FastaSequence( *pSeqNew );
//
//            setNewSeqsOnly.Append( pSeqNewCopy );
//            
//            //
//            mapNewCOntigSrcCtgInfo.insert( map<FastaSequence *, vector< pair<FastaSequence *, int> > > :: value_type( pSeqNew,  vecSeqsCombo) );
//            
////cout << "HAVING ONE NEW CONTIGS\n";
////pSeqNewCopy->printFasta(cout);
////cout << endl;
//        }
//        
//    }
//}

// used for assembly: contig 1 can be reverse-complemented
const string MODE_1_2 = "12";
const string MODE_2_1 = "21";
//const string MODE_1_REV2 = "1R2";
//const string MODE_REV2_1 = "R21";


void* ContigsCompactor::threadMergeContigV2(void *ptr)
{
//pthread_t tid = pthread_self(); //thread id
	ContigsCompactor* pThis = (ContigsCompactor*)ptr;
	
	long nNeedCheck = ContigsCompactor::tempMergeInfo.size();
	while (true)
	{
		int i=-1, j=-1;
		pthread_mutex_lock(&merge_mutex);
		if (vistPos < nNeedCheck)
		{
			i = ContigsCompactor::tempMergeInfo[vistPos].first;
			j = ContigsCompactor::tempMergeInfo[vistPos].second;
			vistPos++;
		}
		pthread_mutex_unlock(&merge_mutex);

		if (i == -1 || j == -1)
			break;

//pthread_mutex_lock (&merge_mutex); //lock to make sure only one thread is writing at the same time 
//cout<<i<<" "<<j<<endl;
//pthread_mutex_unlock(&merge_mutex);//unlock make sure
//continue;

		ContigsCompactorAction ccAct;
		//bool fMatch = false;
		string asmMode;
		int fMatch = pThis->Evaluate(ContigsCompactor::vfs[i], ContigsCompactor::vfs[j], ccAct);
		if (fMatch == OVERLAP_LARGER_MINLEN)
		{
			//
			if ( (ccAct.GetPosEndSeq1() + ccAct.GetOneEndClipLenth()) !=  ContigsCompactor::vfs[i]->size())
			{
				// second go first 
				asmMode = MODE_2_1;
				//vecSeqsCombo = vecSeqs2;
				//std::copy (vecSeqs1.begin(), vecSeqs1.end(), std::back_inserter(vecSeqsCombo));
			}
			else
			{
				// first go first
				asmMode = MODE_1_2;
				//vecSeqsCombo = vecSeqs1;
				//std::copy (vecSeqs2.begin(), vecSeqs2.end(), std::back_inserter(vecSeqsCombo));
			}
		}

		// YW: we don't allow containment; they don't form edges
		if (fMatch == OVERLAP_LARGER_MINLEN && ccAct.IsContainment() == false)
		{
			double pathLen = -1.0*ccAct.GetOverlapSize();           // we want to connect two nodes using the shortest total seq length
			
			pthread_mutex_lock(&merge_write_mutex);
			ConnectionInfo coninfo;
			coninfo.lenth = pathLen;
			coninfo.mode = asmMode;
			coninfo.x = i;
			coninfo.y = j;
			ContigsCompactor::vConnectInfo.push_back(coninfo);
			pthread_mutex_unlock(&merge_write_mutex);
		}
	}

//pthread_mutex_lock (&merge_mutex); //lock to make sure only one thread is writing at the same time
//cout<< tid <<" returned!"<<endl;
//pthread_mutex_unlock(&merge_mutex);//unlock make sure
	
	pthread_exit(NULL);
}


void ContigsCompactor::runMultiThreadMergeV2(int minNumKmerSupport, int T)
{
	int contigSize = ContigsCompactor::numOfContigs;
	ContigsCompactor::vistPos = 0;

	pthread_t* pid = new pthread_t[T];
	for (int i = 0; i < T; i++)
	{
		int ret = pthread_create(&pid[i], NULL, ContigsCompactor::threadMergeContigV2, (void *)this);
		if (ret)
		{
			cout << "Create pthread error!" << endl;
			return;
		}
	}

	for (int i = 0; i < T; i++) //wait for all threads finishing
	{
		pthread_join(pid[i], NULL);

	}
	delete pid;

	pthread_mutex_destroy(&merge_mutex); 
	pthread_mutex_destroy(&merge_write_mutex);
}


int ContigsCompactor::addEdges()
{
	int contigSize = ContigsCompactor::numOfContigs;

	long connectsize = ContigsCompactor::vConnectInfo.size();
	for (long icnt = 0; icnt < connectsize; icnt++)
	{
		ConnectionInfo coninfo = ContigsCompactor::vConnectInfo[icnt];
		int i = coninfo.x;
		int j = coninfo.y;

		if (fVerbose == true)
		{
			cout << " One contigs merged: number of contigs: " << ContigsCompactor::vfs.size() << endl;
			cout << "Contigs: ";
			ContigsCompactor::vfs[i]->printFasta(cout);
			cout << " can be merged with contig: ";
			ContigsCompactor::vfs[j]->printFasta(cout);
			cout << endl;//cout << "RESULT: " << ccAct.GetMerged() << endl;
		}

		GraphNodeRefExt *pnodei = ContigsCompactor::mapContigToGraphNode[ContigsCompactor::vfs[i]];
		GraphNodeRefExt *pnodej = ContigsCompactor::mapContigToGraphNode[ContigsCompactor::vfs[j]];
		string asmMode = coninfo.mode;
		double pathLen = coninfo.lenth;

		if (asmMode == "")
			continue;

		if (asmMode == MODE_1_2)
		{
			// add one edge from node i to j
			pnodei->AddNgbrRef(pnodej, NULL, asmMode, pathLen);
		}
		else if (asmMode == MODE_2_1)
		{
			//pthread_mutex_lock (&ContigsCompactor::merge_mutex); //lock to make sure only one thread is writing at the same time 
			pnodej->AddNgbrRef(pnodei, NULL, asmMode, pathLen);
			//pthread_mutex_unlock(&ContigsCompactor::merge_mutex);//unlock make sure only
		}
		else
		{
			THROW("Fatal error.");
		}
	}

}


void ContigsCompactor :: CompactVer3( MultiFastqSeqs &listContigs, const char* fileScaffoldInfo,  int miniSupportPairCutOff)
{
    // each contig has a node in the graph; YW: to address the issue of reverse complement, create another node (name as <name>_R)
    // create mapping from contig ptr to node
    // create reverse-complement strings 
	//clock_t tStart = clock();

    vector< FastaSequence *> listRevCompContigs;
    map<FastaSequence *, FastaSequence *> mapRevCompContigs;
    for(int i=0; i<(int)listContigs.GetNumOfSeqs(); ++i)
    {
        FastaSequence *pseq = new FastaSequence( *listContigs.GetSeq(i) );
        string strName = listContigs.GetSeq(i)->name();
        strName += "_R";
        pseq->SetName( strName.c_str() );
        pseq->RevsereComplement();
        listRevCompContigs.push_back(pseq);
        mapRevCompContigs.insert( map<FastaSequence *, FastaSequence *> :: value_type( pseq, listContigs.GetSeq(i) ) );
    }
    
    // create a list of contigs to use (incl. reverse comp.)
    vector<FastaSequence *> listContigsPtrInclRevComp;
    for(int i=0; i<(int)listContigs.GetNumOfSeqs(); ++i)
    {
        listContigsPtrInclRevComp.push_back( listContigs.GetSeq(i) );
        listContigsPtrInclRevComp.push_back( listRevCompContigs[i] );
    }
    
    // create nodes
    AbstractGraph graphContigs;
    //map< FastaSequence *, GraphNodeRefExt * > mapContigToGraphNode;
    for(int j=0; j<(int)listContigsPtrInclRevComp.size(); ++j)
    {
        GraphNodeRefExt *gn = new GraphNodeRefExt;
        gn->SetRef( listContigsPtrInclRevComp[j] );
        gn->SetExt( listContigsPtrInclRevComp[j]->name() );
        gn->SetName( listContigsPtrInclRevComp[j]->name() );
        graphContigs.AddNode(gn);
        ContigsCompactor::mapContigToGraphNode.insert( map< FastaSequence *, GraphNodeRefExt * > :: value_type(listContigsPtrInclRevComp[j], gn) );
    }
    // also create a mapping between nodes that are reverse-complement to each other 
    map<AbstractGraphNode *, AbstractGraphNode *> mapListRevCompNodes;
    for( map<FastaSequence *, FastaSequence *> :: iterator it = mapRevCompContigs.begin(); it != mapRevCompContigs.end(); ++it )
    {
        //
        if( ContigsCompactor::mapContigToGraphNode.find(it->first) == ContigsCompactor::mapContigToGraphNode.end()  ||
           ContigsCompactor::mapContigToGraphNode.find(it->second) == ContigsCompactor::mapContigToGraphNode.end())
        {
            THROW("FATAL ERROR");
        }
        AbstractGraphNode *pn1 = ContigsCompactor::mapContigToGraphNode[it->first];
        AbstractGraphNode *pn2 = ContigsCompactor::mapContigToGraphNode[it->second];
        mapListRevCompNodes.insert( map<AbstractGraphNode *, AbstractGraphNode *>  :: value_type( pn1, pn2 ) );
        mapListRevCompNodes.insert( map<AbstractGraphNode *, AbstractGraphNode *>  :: value_type( pn2, pn1 ) );
    }
    
    if(fVerbose )
    {
        cout << "The number of nodes (incl. reverse complements): " << graphContigs.GetNumNodes() << endl;
    }
    
//cout << "The number of nodes (incl. reverse complements): " << graphContigs.GetNumNodes() << endl;
 
	int contigSize = listContigsPtrInclRevComp.size();
	ContigsCompactor::numOfContigs = contigSize;
	ContigsCompactor::tempMergeInfo.clear();
	ContigsCompactor::vConnectInfo.clear();

	// create a list of quickchecker
	// construct all the quick checkers
	MultiThreadQuickChecker multiChecker(contigSize);
	for (int j = 0; j<(int)listContigsPtrInclRevComp.size(); ++j)
	{
		QuickCheckerContigsMatch qchecker(listContigsPtrInclRevComp[j], kmerLenQuick);
		MultiThreadQuickChecker::listQuickCheckers.push_back(qchecker);
		ContigsCompactor::vfs.push_back(listContigsPtrInclRevComp[j]);
	}

	multiChecker.runMultiThreadChecker(numOfThreads);
//cout << "Multiple-check finished" << endl;
//cout << "No. of pairs need to further check is: " << ContigsCompactor::tempMergeInfo.size() << endl;
	runMultiThreadMergeV2(1, numOfThreads);
//cout << "Multiple-merge finished" << endl;
	addEdges();


	vector< std::pair<int, int> >().swap(ContigsCompactor::tempMergeInfo);
	vector<ConnectionInfo>().swap(ContigsCompactor::vConnectInfo);


    if( fVerbose )
    {
        //cout << "The number of nodes: " << graphContigs.GetNumNodes() << endl;
		cout << "The number of dges: " << graphContigs.GetNumEdges() << endl;
    }
    
#if 0
    // output list of SCCs
    vector< set<AbstractGraphNode *> > setSCCs;
    graphContigs.SCC( setSCCs );
    cout << "The number of strongly connected components: " << setSCCs.size() << endl;
//#if 0
    for( int i=0; i<(int)setSCCs.size(); ++i )
    {
        cout << setSCCs[i].size() << "  ";
        for( set<AbstractGraphNode *> :: iterator it = setSCCs[i].begin(); it != setSCCs[i].end(); ++it)
        {
            GraphNodeRefExt *pnoderef = (GraphNodeRefExt *)(*it);
            //FastaSequence *pContig = (FastaSequence *) ( pnoderef->GetRef() );
            //cout << pContig->name();
            //cout << "  ";
            pnoderef->Dump();
        }
        //cout << endl;
    }
    cout << endl;
//#endif
//exit(1);
#endif
    
//printf("Time taken for pairwise comparison: %.2fs\n", (double)(clock() - tStart)/CLOCKS_PER_SEC);

//clock_t tStart_path = clock();

// output graph to a file
const char fileGML[] = "tmp.gml";
graphContigs.OutputGML( fileGML );
//cout << "GML file outputted....\n";
//exit(1);
    
    // now find paths
    set<vector<AbstractGraphNode *> > setPaths;
    //graphContigs.FindSimplePaths( setPaths, maxContigPathLen, maxCountPerContigInPaths);
    //graphContigs.FindSimplePathsBoundedLength( setPaths, maxContigPathLen );
	graphContigs.FindSimplePathsTopSort( setPaths, maxCountPerContigInPaths ); //need to check ////////////////////////////////////

	if (fVerbose)
	{
		cout << "Number of paths found: " << setPaths.size() << endl;
		for (set<vector<AbstractGraphNode *> > ::iterator it = setPaths.begin(); it != setPaths.end(); ++it)
		{
			for (int i = 0; i < it->size(); i++)
			{
				cout << it->at(i)->GetName() << " ";
			}
			cout << endl;
			//cout << it->size() << "  ";
		}
		//cout << endl;
	}
//exit(1); 
    
    // remove duplicate path (by reverse complement)
    RemoveDupRevCompPaths(setPaths, mapListRevCompNodes);
    

    // create assembled path
    static int contigNumNext = 1;
    for( set<vector<AbstractGraphNode *> > :: iterator it = setPaths.begin(); it != setPaths.end(); ++it )
    {
        
// dump out a path
#if 0
for(int i=0; i<(int)it->size(); ++i)
{
GraphNodeRefExt *pnref = dynamic_cast<GraphNodeRefExt *>( (*it)[i] );
FastaSequence *pSeq = ( FastaSequence * )pnref->GetRef();
cout << " " << pSeq->name();
}
cout << endl;
#endif
        
        string strContigsSub;
        for(int i=0; i<(int)it->size(); ++i)
        {
            GraphNodeRefExt *pnref = dynamic_cast<GraphNodeRefExt *>( (*it)[i] );
            FastaSequence *pSeq = ( FastaSequence * )pnref->GetRef();
            strContigsSub += " ";
            strContigsSub += pSeq->name();
        }

        
        if( it->size() > 1 )
        {
            // this is a new contig
            string strContigNew = FormMergedSeqFromPath( *it );
            char nameContig[100];
            sprintf(nameContig, "NEW_CONTIG_MERGE_%d", contigNumNext++);
            FastaSequence seqNew;
            seqNew.SetName( nameContig );
            seqNew.SetSeq( strContigNew );
            //cout << nameContig << endl;
            //cout << strContigNew << endl;
            seqNew.printFasta(cout);
            
            string nameContigstr = nameContig;
            
            mapNewContigNameToPath.insert( map<string,string> :: value_type( nameContigstr, strContigsSub ) );
        }

    }
    
    // cleanup
    for(int i=0; i<(int)listRevCompContigs.size(); ++i)
    {
        delete listRevCompContigs[i];
    }

}

//*******************************************************************************************

MultiThreadQuickChecker::MultiThreadQuickChecker(int num) 
{
	this->numOfContigs = num;
}

void MultiThreadQuickChecker::runMultiThreadChecker(int T)
{
	vector< pair< pair<int, int>, pair<int, int> > > vprang;
	vprang.clear();

	long ncontigs = (long)numOfContigs;
	long long totalCells = ncontigs*ncontigs;
	totalCells -= ncontigs;
	totalCells /= 2;

	long avrg = totalCells / T;
	long ncnt = 0;
	int pre_i = 0;
	int pre_j = -1;
	for (int i = 0; i<MultiThreadQuickChecker::numOfContigs; i++)
	{
		for (int j = i; j<MultiThreadQuickChecker::numOfContigs; j++)
		{			
			ncnt++;
			if (ncnt == avrg)
			{
				pair<int, int> lprang;
				lprang.first = pre_i;
				lprang.second = pre_j;

				pair<int, int> rprang;
				rprang.first = i;
				rprang.second = j;

				pair< pair<int, int>, pair<int, int> > prang;
				prang.first = lprang;
				prang.second = rprang;

				vprang.push_back(prang);
				pre_i = i;
				pre_j = j;
				ncnt = 0;
			}
		}
	}


	if (vprang.size()<T)
	{
		cout << "Arrange error! " <<vprang.size()<<" "<<T<< endl;
		return;
	}
	vprang[T - 1].second.first = MultiThreadQuickChecker::numOfContigs - 1;
	vprang[T - 1].second.second = MultiThreadQuickChecker::numOfContigs - 1;

///////////////////////////////
//for (int i = 0; i < T; i++)
//{
//	cout << vprang[i].first.first << " " << vprang[i].first.second << " " << vprang[i].second.first << " " << vprang[i].second.second << endl;
//}

	pthread_t* pid = new pthread_t[T];
	for (int i = 0; i<T; i++)
	{
		//create a new thread and pass parameters 
		int ret = pthread_create(&pid[i], NULL, MultiThreadQuickChecker::threadQuickCheck, (void *)&vprang[i]);
		if (ret)
		{
			cout << "Create pthread error!" << endl;
			return;
		}
	}
	//cout<<"waiting for all threads finished"<<endl;
	for (int i = 0; i<T; i++) //wait for all threads finishing
		pthread_join(pid[i], NULL);
	delete pid;

	pthread_mutex_destroy(&check_mutex);
}


void* MultiThreadQuickChecker::threadQuickCheck(void* ptr)
{
	pair< pair<int, int>, pair<int, int> > rang = *((pair< pair<int, int>, pair<int, int> >*)ptr);
	pair<int, int> lprang = rang.first;
	pair<int, int> rprang = rang.second;

	int i = lprang.first;
	int j = lprang.second + 1;

	while (i <= rprang.first)
	{
		if ((i == rprang.first) && (j>rprang.second))
			break;

		if (j >= MultiThreadQuickChecker::numOfContigs)
		{
			i++;
			j = i;
			continue;
		}

		if (MultiThreadQuickChecker::listQuickCheckers[i].IsMatchFeasible(ContigsCompactor::vfs[j]) == true)
		{
			pthread_mutex_lock(&check_mutex);
			ContigsCompactor::tempMergeInfo.push_back(std::make_pair(i, j));
			pthread_mutex_unlock(&check_mutex);
		}

		j++;
	}

	pthread_exit(NULL);
}

//*******************************************************************************************


// graph-based contig merging
//void ContigsCompactor :: CompactVer3( MultiFastqSeqs &listContigs, const char* fileScaffoldInfo,  int miniSupportPairCutOff)
//{
//    // each contig has a node in the graph; YW: to address the issue of reverse complement, create another node (name as <name>_R)
//    // create mapping from contig ptr to node
//    // create reverse-complement strings
//    vector< FastaSequence *> listRevCompContigs;
//    map<FastaSequence *, FastaSequence *> mapRevCompContigs;
//    for(int i=0; i<(int)listContigs.GetNumOfSeqs(); ++i)
//    {
//        FastaSequence *pseq = new FastaSequence( *listContigs.GetSeq(i) );
//        string strName = listContigs.GetSeq(i)->name();
//        strName += "_R";
//        pseq->SetName( strName.c_str() );
//        pseq->RevsereComplement();
//        listRevCompContigs.push_back(pseq);
//        mapRevCompContigs.insert( map<FastaSequence *, FastaSequence *> :: value_type( pseq, listContigs.GetSeq(i) ) );
//    }
//    
//    // create a list of contigs to use (incl. reverse comp.)
//    vector<FastaSequence *> listContigsPtrInclRevComp;
//    for(int i=0; i<(int)listContigs.GetNumOfSeqs(); ++i)
//    {
//        listContigsPtrInclRevComp.push_back( listContigs.GetSeq(i) );
//        listContigsPtrInclRevComp.push_back( listRevCompContigs[i] );
//    }
//    
//    // create nodes
//    AbstractGraph graphContigs;
//    map< FastaSequence *, GraphNodeRefExt * > mapContigToGraphNode;
//    for(int j=0; j<(int)listContigsPtrInclRevComp.size(); ++j)
//    {
//        GraphNodeRefExt *gn = new GraphNodeRefExt;
//        gn->SetRef( listContigsPtrInclRevComp[j] );
//        gn->SetExt( listContigsPtrInclRevComp[j]->name() );
//        gn->SetName( listContigsPtrInclRevComp[j]->name() );
//        graphContigs.AddNode(gn);
//        mapContigToGraphNode.insert( map< FastaSequence *, GraphNodeRefExt * > :: value_type(listContigsPtrInclRevComp[j], gn) );
//    }
//    // also create a mapping between nodes that are reverse-complement to each other 
//    map<AbstractGraphNode *, AbstractGraphNode *> mapListRevCompNodes;
//    for( map<FastaSequence *, FastaSequence *> :: iterator it = mapRevCompContigs.begin(); it != mapRevCompContigs.end(); ++it )
//    {
//        //
//        if( mapContigToGraphNode.find(it->first) == mapContigToGraphNode.end()  ||
//           mapContigToGraphNode.find(it->second) == mapContigToGraphNode.end())
//        {
//            THROW("FATAL ERROR");
//        }
//        AbstractGraphNode *pn1 = mapContigToGraphNode[it->first];
//        AbstractGraphNode *pn2 = mapContigToGraphNode[it->second];
//        mapListRevCompNodes.insert( map<AbstractGraphNode *, AbstractGraphNode *>  :: value_type( pn1, pn2 ) );
//        mapListRevCompNodes.insert( map<AbstractGraphNode *, AbstractGraphNode *>  :: value_type( pn2, pn1 ) );
//    }
//
//
//    if(fVerbose )
//    {
//        cout << "The number of nodes (incl. reverse complements): " << graphContigs.GetNumNodes() << endl;
//    }
//    
//    // create a list of quckchecker 
//    // construct all the quick checkers
//    vector< QuickCheckerContigsMatch>  listQuickCheckers;
//    for(int j=0; j<(int)listContigsPtrInclRevComp.size(); ++j)
//    {
//        // now perform quick check
//        QuickCheckerContigsMatch qchecker(listContigsPtrInclRevComp[j], kmerLenQuick);
//        listQuickCheckers.push_back(qchecker);
//    }
//    
//	//CC: load scaffold information 
//	MapScaffoldInfo mapScaffoldInfo;
//	if(fileScaffoldInfo != "")
//	{
//		loadScaffoldInfo(fileScaffoldInfo, mapScaffoldInfo, miniSupportPairCutOff);
//	}
//
////cout<<"Finished loading scaffold information!!!"<<endl;
//    //fCont = false;
//    vector< vector< pair<FastaSequence *, int> > > listVecSeqsCombo;
//    
//    for(int i=0; i<(int)listContigsPtrInclRevComp.size(); ++i)
//    {
//        GraphNodeRefExt *pnodei = mapContigToGraphNode[ listContigsPtrInclRevComp[i] ];        
//        int numEdges = 0;
//
//        // now check each of the following ones
//        for(int j=i+1; j<(int)listContigsPtrInclRevComp.size(); ++j)
//        {
//            GraphNodeRefExt *pnodej = mapContigToGraphNode[ listContigsPtrInclRevComp[j] ];
//            
////cout << "Now evaluating contigs: ";
////listContigsPtrInclRevComp[i]->printFasta(cout);
////listContigsPtrInclRevComp[j]->printFasta(cout);
//
//            ContigsCompactorAction ccAct;
//            //bool fMatch = false;
//			int fMatch=0;
//            string asmMode;
//            if( listQuickCheckers[i].IsMatchFeasible( listContigsPtrInclRevComp[j] ) == true )
//            {
////cout << "Pass quick checking\n"; 
//                fMatch = Evaluate( listContigsPtrInclRevComp[i], listContigsPtrInclRevComp[j], ccAct);
//
//                if( fMatch != OVERLAP_SMALLER_MINLENSCAFFOLD)
//                {
////cout << "MATCH\n"; 
//                    //
//                    if( ccAct.GetPosEndSeq1() <  listContigsPtrInclRevComp[i]->size() )
//                    {
//                        // second go first
//                        asmMode = MODE_2_1;
//                        //vecSeqsCombo = vecSeqs2;
//                        //std::copy (vecSeqs1.begin(), vecSeqs1.end(), std::back_inserter(vecSeqsCombo));
//                    }
//                    else
//                    {
//                        // first go first
//                        asmMode = MODE_1_2;
//                        //vecSeqsCombo = vecSeqs1;
//                        //std::copy (vecSeqs2.begin(), vecSeqs2.end(), std::back_inserter(vecSeqsCombo));
//                    }
//                }
//            }
//
//			/*
//			const int OVERLAP_SMALLER_MINLENSCAFFOLD=0;
//const int OVERLAP_IN_RANGE=1;
//const int OVERLAP_LARGER_MINLEN=2;
//			*/
//
//			if(fMatch==OVERLAP_SMALLER_MINLENSCAFFOLD)
//				continue;
//
//			//CC: Then check whether there is a connection between thes two contigs 
//			bool bconnected=false;
//			string contigNamei=pnodei->GetName();
//			string contigNamej=pnodej->GetName();
//			//CC: check whether there is a connection between the two node 
//			if(mapScaffoldInfo.size()>0)
//			{
//				if(mapScaffoldInfo.find(contigNamei) != mapScaffoldInfo.end())
//				{
//					if(mapScaffoldInfo[contigNamei].find(contigNamej) != mapScaffoldInfo[contigNamei].end())
//						bconnected=true;
//				}
//
//				if(bconnected==false)
//				{
//					if(mapScaffoldInfo.find(contigNamej) != mapScaffoldInfo.end())
//					{
//						if(mapScaffoldInfo[contigNamej].find(contigNamei) != mapScaffoldInfo[contigNamej].end())
//							bconnected=true;
//					}
//				}
//			}
////cout<<"Begin to check whether a qualified connection"<<endl;
//			//CC: if no connection from scaffold, also doesn't pass quick checker, then do not add the edge
//			if(bconnected==false && fMatch==OVERLAP_IN_RANGE)
//			{
//				continue;
//			}
//
//            // YW: we don't allow containment; they don't form edges 
//            if( ccAct.IsContainment() == false)
//            {
//                
//                if( fVerbose == true )
//                {
//                    cout << "One contigs merged: number of contigs: " << listContigsPtrInclRevComp.size() << endl;
//                    cout << "Contigs: ";
//                    listContigsPtrInclRevComp[i]->printFasta(cout);
//                    cout << " can be merged with contig: ";
//                    listContigsPtrInclRevComp[j]->printFasta(cout);
//                    cout << "RESULT: " << ccAct.GetMerged() << endl;
//                }
//                ++numEdges;
//                
//                // add edge based on assembly mode
//                //double pathLen = -1.0*ccAct.GetMergedLen();
//                double pathLen = -1.0*ccAct.GetOverlapSize();// we want to connect two nodes using the shortest total seq length
//                //double pathLen = 1.0;
//                //if( pathLen > 0 )
//                //{
//                //    cout << "Something wrong: overlap lenght cannot be negative\n";
//                //}
//                //double pathLen = -1.0;
//                if( asmMode == MODE_1_2 )
//                {
//                    // add one edge from node i to j
//                    pnodei->AddNgbrRef( pnodej, NULL, asmMode, pathLen );
//                }
//                else if( asmMode == MODE_2_1 )
//                {
//                    pnodej->AddNgbrRef( pnodei, NULL, asmMode, pathLen );
//                }
//                else
//                {
//                    THROW("Fatal error.");
//                }                
//            }
//            
//        }//end of for 
//        if( fVerbose )
//        {
//            cout << " i = " << i  << ": Number of found edges: " << numEdges << endl;
//        }
//    }
//
//    if( fVerbose )
//    {
//        //cout << "The number of nodes: " << graphContigs.GetNumNodes() << endl;
//        cout << "The number of dges: " << graphContigs.GetNumEdges() << endl;
//    }
//    
//#if 0
//    // output list of SCCs
//    vector< set<AbstractGraphNode *> > setSCCs;
//    graphContigs.SCC( setSCCs );
//    cout << "The number of strongly connected components: " << setSCCs.size() << endl;
////#if 0
//    for( int i=0; i<(int)setSCCs.size(); ++i )
//    {
//        cout << setSCCs[i].size() << "  ";
//        for( set<AbstractGraphNode *> :: iterator it = setSCCs[i].begin(); it != setSCCs[i].end(); ++it)
//        {
//            GraphNodeRefExt *pnoderef = (GraphNodeRefExt *)(*it);
//            //FastaSequence *pContig = (FastaSequence *) ( pnoderef->GetRef() );
//            //cout << pContig->name();
//            //cout << "  ";
//            pnoderef->Dump();
//        }
//        //cout << endl;
//    }
//    cout << endl;
////#endif
////exit(1);
//#endif
//    
//// output graph to a file
//const char fileGML[] = "tmp.gml";
//graphContigs.OutputGML( fileGML );
////cout << "GML file outputted....\n";
////exit(1);
//    
//    
//    // now find paths    
//    set<vector<AbstractGraphNode *> > setPaths;
//    //graphContigs.FindSimplePaths( setPaths, maxContigPathLen, maxCountPerContigInPaths);
//    //graphContigs.FindSimplePathsBoundedLength( setPaths, maxContigPathLen );
//    graphContigs.FindSimplePathsTopSort( setPaths, maxCountPerContigInPaths );
////cout << "Number of paths found: " << setPaths.size() << ", lengths are: ";
////for( set<vector<AbstractGraphNode *> > :: iterator it = setPaths.begin(); it != setPaths.end(); ++it)
////{
////cout << it->size() << "  ";
////}
////cout << endl;
////exit(1);
//    
//    // remove duplicate path (by reverse complement) 
//    RemoveDupRevCompPaths(setPaths, mapListRevCompNodes);
//    
//    // create assembled path
//    static int contigNumNext = 1;
//    for( set<vector<AbstractGraphNode *> > :: iterator it = setPaths.begin(); it != setPaths.end(); ++it )
//    {
//        
//// dump out a path 
//#if 0
//for(int i=0; i<(int)it->size(); ++i)
//{
//GraphNodeRefExt *pnref = dynamic_cast<GraphNodeRefExt *>( (*it)[i] );
//FastaSequence *pSeq = ( FastaSequence * )pnref->GetRef();
//cout << " " << pSeq->name();
//}
//cout << endl;
//#endif
//        
//        string strContigsSub;
//        for(int i=0; i<(int)it->size(); ++i)
//        {
//            GraphNodeRefExt *pnref = dynamic_cast<GraphNodeRefExt *>( (*it)[i] );
//            FastaSequence *pSeq = ( FastaSequence * )pnref->GetRef();
//            strContigsSub += " ";
//            strContigsSub += pSeq->name();
//        }
//
//        
//        if( it->size() > 1 )
//        {
//            // this is a new contig
//            string strContigNew = FormMergedSeqFromPath( *it );
//            char nameContig[100];
//            sprintf(nameContig, "NEW_CONTIG_MERGE_%d", contigNumNext++);
//            FastaSequence seqNew;
//            seqNew.SetName( nameContig );
//            seqNew.SetSeq( strContigNew );
//            //cout << nameContig << endl;
//            //cout << strContigNew << endl;
//            seqNew.printFasta(cout);
//            
//            string nameContigstr = nameContig;
//            
//            mapNewContigNameToPath.insert( map<string,string> :: value_type( nameContigstr, strContigsSub ) );
//        }
//
//    }
//    
//    // cleanup
//    for(int i=0; i<(int)listRevCompContigs.size(); ++i)
//    {
//        delete listRevCompContigs[i];
//    }
//
//}

void ContigsCompactor :: RemoveDupRevCompPaths( set<vector<AbstractGraphNode *> > &setPaths, map<AbstractGraphNode *, AbstractGraphNode *> &mapListRevCompNodes )
{
//return;
    // remove duplicate paths caused by reverse-complement
    set<vector<AbstractGraphNode *> > setPathsCleaned;
    
    for( set<vector<AbstractGraphNode *> > :: iterator it = setPaths.begin(); it != setPaths.end(); ++it )
    {
        // create the reverse complement one
        vector<AbstractGraphNode *> pathRC;
        for(int i=(int)it->size()-1; i>=0; --i)
        {
            pathRC.push_back( mapListRevCompNodes[  (*it)[i] ] );
        }
        bool fDup = false;
        for(  set<vector<AbstractGraphNode *> > :: iterator it2 = setPaths.begin(); it2 != setPaths.end() && it2 != it; ++it2)
        {
            //
            if( *it2 == pathRC )
            {
                fDup = true;
                break;
            }
        }
        if( fDup == false )
        {
            // add to the result
            setPathsCleaned.insert(*it);
        }
    }
    
    setPaths = setPathsCleaned;
}

string ContigsCompactor :: FormMergedSeqFromPath( const vector<AbstractGraphNode *> &listPathNodes  )
{
    GraphNodeRefExt *pnref = dynamic_cast<GraphNodeRefExt *>( listPathNodes[0]);
    FastaSequence *pSeq = ( FastaSequence * )pnref->GetRef();
    
    // construct a new sequence
    FastaSequence seqMerg( *pSeq );
    for(int i=1; i<(int)listPathNodes.size(); ++i)
    {
        GraphNodeRefExt *pnrefi = dynamic_cast<GraphNodeRefExt *>( listPathNodes[i]);
        FastaSequence *pSeqi = ( FastaSequence * )pnrefi->GetRef();


        GraphNodeRefExt *pnrefpre = dynamic_cast<GraphNodeRefExt *>( listPathNodes[i-1]);
        FastaSequence *pSeqipre = ( FastaSequence * )pnrefpre->GetRef();
        
#if 0
cout << "Prior contig: " << pSeqipre->name() << endl;
cout << "Current contig: " << pSeqi->name() << endl;
ContigsCompactorAction ccActPre;
bool fMatch1 = Evaluate( pSeqipre, pSeqi, ccActPre );
if( fMatch1 == false )
{
FastaSequence seqRevComp( *pSeqipre );
seqRevComp.RevsereComplement();
fMatch1 = Evaluate( &seqRevComp, pSeqi, ccActPre );
if( fMatch1 == false )
{
cout << "FATAL ERROR: the path seems to be wrong.\n";
exit(1);
}
}
#endif
        ContigsCompactorAction ccAct;
        //bool fMatch = Evaluate( &seqMerg, pSeqi, ccAct, true);
        int fMatch = Evaluate(&seqMerg, pSeqi, ccAct, true);
		if( fMatch != OVERLAP_LARGER_MINLEN)
        {
            // do the other way
            //FastaSequence seqRevComp( seqMerg );
            //FastaSequence seqRevComp( *pSeqi );
            //seqRevComp.RevsereComplement();
            //fMatch = Evaluate( &seqRevComp, pSeqi, ccAct);
            //fMatch = Evaluate( &seqMerg, &seqRevComp, ccAct);
            
            if( fVerbose == true )
            {
                cout << "************************WARNING: early terminate\n";
                cout << "Current merging seq: " << seqMerg.c_str() << endl;
                cout << "current seq: ";
                pSeqi->printFasta(cout);
                cout << "prior seq: ";
                pSeqipre->printFasta(cout);
            }
            return seqMerg.c_str();
        }
        seqMerg.SetSeq( ccAct.GetMerged() );
    }
    return seqMerg.c_str();
}


void ContigsCompactor :: OutputContigsInfo(const char *fileOut)
{
    //
    ofstream outfile( fileOut );
    if( outfile.is_open() == false )
    {
        cout << "Can not open file: " << fileOut << endl;
        exit(1);
    }
    
    // write out all that is stored
    for( map<FastaSequence *, vector< pair<FastaSequence *, int> > > :: iterator it = mapNewCOntigSrcCtgInfo.begin(); it != mapNewCOntigSrcCtgInfo.end(); ++it )
    {
        //
        outfile << it->first->name() << "  ";
        for( int i=0; i<(int)it->second.size(); ++i )
        {
            outfile << it->second[i].first->name();
            outfile << " ";
            outfile << "(" << it->second[i].second << ")    ";
        }
        outfile << endl;
    }
    
    outfile.close();
}

void ContigsCompactor :: OutputContigsInfoVer2(const char *fileOut)
{
    ofstream outfile( fileOut );
    if( outfile.is_open() == false )
    {
        cout << "Can not open file: " << fileOut << endl;
        exit(1);
    }
    
    // write out all that is stored
    for( map<string, string> :: iterator it= mapNewContigNameToPath.begin(); it != mapNewContigNameToPath.end(); ++it )
    {
        //
        outfile << it->first << "  " << it->second;
        outfile << endl;
    }
    
    outfile.close();

}

/*
Return:
	0: no Overlap or Overlap < minOverlapLenWithScaffold;
	1: Overlap in range [minOverlapLenWithScaffold, minOverlapLen);
	2: Overlap > minOverlapLen
*/
int ContigsCompactor::Evaluate(FastaSequence *pSeq1, FastaSequence *pSeq2, ContigsCompactorAction &resCompact, bool fRelax)
{
	// perform simple DP to compare the two sequences
	if (fVerbose == true)
	{
		pthread_mutex_lock(&merge_mutex); //lock to make sure only one thread is writing at the same time 
		pthread_t tid = pthread_self(); //thread id
		cout << tid << " Evaluating..." << endl;
		cout << " Seq1: ";
		pSeq1->printFasta(cout);
		cout << ", Seq2: ";
		pSeq2->printFasta(cout);
		cout << endl;
		pthread_mutex_unlock(&merge_mutex);//unlock make sure
	}
	// evalaute whether the area can be filled by the passed-in repeat
	// use dynamic programming; use a simple score as follows
	// match: 1, mismatch/indel/: -1
	// middle gap: no penalty if length is within some threshold (+/- say 20%)
	// otherwise: -inf
	// tbl[x,y]: x is coordinate in repeats, y: scalfold (relative to the start)
	// define scoring scheme (simple for now)
	

	int scoreMatch = 1;
	//int scoreMismatch = -1;
	//int scoreIndel = -1;

	int szSeq1 = (int)pSeq1->size();
	vector<vector<double> > tblScore(szSeq1 + 1);
	vector<vector<pair<int, int> > > tblTraceBack(szSeq1 + 1);
	int szSeq2 = (int)pSeq2->size();
	for (int i = 0; i<(int)tblScore.size(); ++i)
	{
		tblScore[i].resize(szSeq2 + 1);
		tblTraceBack[i].resize(szSeq2 + 1);
	}
	// init: first row is all-0,
	// meaning alignment can start from any point in the working zone
	for (int j = 0; j<(int)tblScore[0].size(); ++j)
	{
		int scoreInit = 0;
		tblScore[0][j] = scoreInit;
		pair<int, int> pp(-1, -1);
		tblTraceBack[0][j] = pp;    // stop tracing
	}
	for (int i = 1; i<(int)tblScore.size(); ++i)
	{
		// initial score: 0 s.t. we allow overlap alignment 
		int scoreCol0 = 0;
		// if over the threshold of end clipping of repeats, forbid
		//if( i > (int)(pRepeatSeq->size()*thresEndClipRepeat)  )
		//{
		//    //
		//    scoreCol0 = MAX_NEG_SCORE;
		//}
		tblScore[i][0] = scoreCol0;
		pair<int, int> pp(-1, -1);
		tblTraceBack[i][0] = pp;    // stop tracing

									//int posRep = i-1;
		for (int j = 1; j<(int)tblScore[i].size(); ++j)
		{
			//cout << "### determine value for cell [" << i << "," << j << "]...\n";
			// need to take special care if the column is right after gap in the scaffold
			// in this, need to consider the previous column (i.e. in scaffold)
			// can be mapped to a much wider range of repeat position (+/-)
			// some gap size
			int matchScoreStep = scoreMismatch;
			if (pSeq1->at(i - 1) == pSeq2->at(j - 1))
			{
				matchScoreStep = scoreMatch;
			}
			//cout << "gapRightBoundZeroBase: " << gapRightBoundZeroBase << ", posRep = " << posRep << "["  << pRepeatSeq->at(posRep) << "]" << ", posScaf = " << posScaf << " [" << pScaffoldSeq->at( posScaf )  << "]" << endl;

			double scoreStep;
			pair<int, int> tbStep(-1, -1);

			// just take the normal DP
			scoreStep = tblScore[i - 1][j - 1] + matchScoreStep;
			tbStep.first = i - 1;
			tbStep.second = j - 1;
			if (scoreStep < tblScore[i - 1][j] + scoreIndel)
			{
				scoreStep = tblScore[i - 1][j] + scoreIndel;
				tbStep.first = i - 1;
				tbStep.second = j;
			}
			if (scoreStep < tblScore[i][j - 1] + scoreIndel)
			{
				scoreStep = tblScore[i][j - 1] + scoreIndel;
				tbStep.first = i;
				tbStep.second = j - 1;
			}

			tblScore[i][j] = scoreStep;
			tblTraceBack[i][j] = tbStep;
			//cout << "#### set value to " << scoreStep << " obtained from " << tbStep.first << ", " << tbStep.second << endl;
		}
	}

	// just find the largest score over the last row/column 
	int scoreMax = MAX_NEG_SCORE;
	int posRowEnd = -1;
	int posColEnd = -1;
	int nclip = -1;

	for (int c = 0; c <= maxOverlapClipLen; c++) //we allow some mismatch at one of the prefix or suffix
	{
		for (int i = 0; i<(int)tblScore.size(); ++i)
		{
			int icol = szSeq2 - c;
			if (icol<0) break;

			if (tblScore[i][icol] > scoreMax)
			{
				scoreMax = tblScore[i][icol];
				posColEnd = icol;
				posRowEnd = i;
				nclip = c;
			}
		}


		for (int j = 0; j <= szSeq2; ++j)
		{
			int irow = szSeq1 - c;
			if (irow<0) break;

			if (tblScore[irow][j] > scoreMax)
			{
				scoreMax = tblScore[irow][j];
				posColEnd = j;
				posRowEnd = irow;
				nclip = c;
			}
		}
	}
	

	int res = 2;
	// if in the relax mode, don't check for scoring
	if (fRelax == false)
	{	
		//const int OVERLAP_SMALLER_MINLENSCAFFOLD=0;
		//const int OVERLAP_IN_RANGE=1;
		//const int OVERLAP_LARGER_MINLEN=2;

		int score_significant = this->IsScoreSignificant(scoreMax, szSeq1, szSeq2, posRowEnd, posColEnd, nclip);
		if (score_significant == OVERLAP_SMALLER_MINLENSCAFFOLD)
			return OVERLAP_SMALLER_MINLENSCAFFOLD;
		else if (score_significant == OVERLAP_IN_RANGE)
			res = OVERLAP_IN_RANGE;
		else
			res = OVERLAP_LARGER_MINLEN;
	}
	
	if (fVerbose == true)
	{
		cout << "IsScoreSignificant returns " << res << endl;
	}

	//#if 0
	// now find trace back
	pair<int, int> tbCur(posRowEnd, posColEnd);
	string mergedSeqTB;

	// first get the clipped one (if any)
	if ((posRowEnd+nclip) != szSeq1 && (posColEnd+nclip) != szSeq2)
	{
		//
		THROW("Fatal error1");
	}

	if ((posColEnd + nclip)==szSeq2 && posRowEnd < szSeq1)
	{
		
		for (int i = szSeq1; i>posRowEnd; --i)
		{
			mergedSeqTB.push_back(pSeq1->at(i - 1));
		}
	}
	if ((posRowEnd + nclip) == szSeq1 && posColEnd < szSeq2)
	{
		//
		for (int i = szSeq2; i>posColEnd; --i)
		{
			mergedSeqTB.push_back(pSeq2->at(i - 1));
		}
	}

	while (tbCur.first > 0 && tbCur.second > 0)
	{
		//cout << "--trace " << tbCur.first << "," << tbCur.second << ": \n" << mergedSeqTB << endl;
		// stop if anything is out-of-scope
		if (tbCur.first < 0 || tbCur.second < 0 || (tbCur.first + tbCur.second == 0))
		{
			// nothing more to trace
			break;
		}

		// first put the two corresponding chars
		// now trace back
		pair<int, int> tbPre = tblTraceBack[tbCur.first][tbCur.second];

		if (tbCur.second > tbPre.second)
		{
			if (tbCur.second > 0)
			{
				int posScfConv = tbCur.second;
				if (posScfConv < 0 || posScfConv > pSeq2->size())
				{
					THROW("Fatal error2");
				}
				//cout << "posScfConv = " << posScfConv << endl;
				mergedSeqTB.push_back(pSeq2->at(posScfConv - 1));
			}
			else
			{
				mergedSeqTB.push_back('-');
			}
		}
		else if (tbCur.first > tbPre.first)
		{
			if (tbCur.first >= 1)
			{
				mergedSeqTB.push_back(pSeq1->at(tbCur.first - 1));
			}
			else
			{
				mergedSeqTB.push_back('-');
			}
		}
		else
		{
			// done
			break;
			//mergedSeqTB.push_back('-');
		}
		tbCur = tbPre;
	}

	bool bcontained = false;
	// get the clipped part on the left
	if (tbCur.first > 0)
	{
		//
		for (int i = tbCur.first; i>0; --i)
		{
			mergedSeqTB.push_back(pSeq1->at(i - 1));
		}
	}
	if (tbCur.second > 0)
	{
		//
		for (int i = tbCur.second; i>0; --i)
		{
			mergedSeqTB.push_back(pSeq2->at(i - 1));
		}
	}


	if ((posRowEnd+nclip) == szSeq1 && tbCur.first == 0)
		bcontained = true;
	if ((posColEnd+nclip) == szSeq2 && tbCur.second == 0)
		bcontained = true;
	

	// this is the traceback result
	reverse(mergedSeqTB.begin(), mergedSeqTB.end());
	//resAlnPairs.first = scafTB;
	//reverse( repTB.begin(), repTB.end() );
	//resAlnPairs.second = repTB;

	//cout << "The merged sequence: " << mergedSeqTB << endl;
	//cout << "posRowEnd: " << posRowEnd << ", length1: " << szSeq1 << ", length2: " << szSeq2 << ", posColEnd: " << posColEnd << endl;
	//resCompact.SetMergedString( mergedSeqTB );
	resCompact.SetPosEndSeq1(posRowEnd);
	resCompact.SetOrigSeqLen(szSeq1, szSeq2);
	resCompact.SetAlnSeqs(pSeq1->c_str(), pSeq2->c_str());
	resCompact.SetDPEnds(posRowEnd, posColEnd);
	resCompact.SetOneEndClipLenth(nclip);
	resCompact.SetContainedFlag(bcontained);
	resCompact.SetMergedStringConcat();
	//#endif

#if 0
	cout << " ^^^^ scoring matrix: \n";
	for (int i = 0; i<(int)tblScore.size(); ++i)
	{
		for (int j = 0; j<(int)tblScore[i].size(); ++j)
		{
			cout << tblScore[i][j] << "  ";
		}
		cout << endl;
	}
	//#endif
	cout << ", scoreMax = " << scoreMax << endl;
#endif
	return res;

}


int ContigsCompactor :: IsScoreSignificant( int scoreMax, int szSeq1, int szSeq2, int rowStart, int colStart, int nclip  ) const
{
    if( fVerbose )
    {
		pthread_mutex_lock(&merge_mutex);//unlock make sure
		pthread_t tid = pthread_self(); //thread id
		cout << tid << " IsScoreSignificant: score = " << scoreMax << ", szSeq1: " << szSeq1 << ", szSeq2: " << szSeq2 << ", rowStart: " << rowStart << ", colStart: " << colStart << endl;
		pthread_mutex_unlock(&merge_mutex);//unlock make sure
	}
    
    // must enforce at least half of overlap
    //int szOverlap = rowStart;
    //if( rowStart == szSeq1)
    //{
    //    szOverlap = colStart;
    //}
	

    int szOverlap0 = min(szSeq1, szSeq2);
    int szOverlap1 = szOverlap0 , szOverlap2=szOverlap0;
    if( rowStart + nclip == szSeq1)
    {
        szOverlap1 = colStart;
    }
    if( colStart + nclip == szSeq2 )
    {
        szOverlap2 = rowStart;
    }
    int szOverlap = min( szOverlap0, min(szOverlap1, szOverlap2) );
    //else
    //{
    //    cout << "FATAL ERROR.\n";
    //    exit(1);
    //}
    
    if( szOverlap < szSeq1*fracMinOverlap && szOverlap < szSeq2*fracMinOverlap )
    {
        return OVERLAP_SMALLER_MINLENSCAFFOLD;
    }
    
    const int MIN_ASM_EXT_LEN = 5;
    
    // also forbid the situation where one contig contains the other
    if( rowStart + nclip == szSeq1  )
    {
        if( colStart+MIN_ASM_EXT_LEN-1 >= szSeq2 )
        {
            return OVERLAP_SMALLER_MINLENSCAFFOLD;
        }
    }
    if( colStart + nclip == szSeq2)
    {
        if( rowStart+MIN_ASM_EXT_LEN-1 >= szSeq1)
        {
            return OVERLAP_SMALLER_MINLENSCAFFOLD;
        }
    }
    
#if 0
    // YW: also forbid merging that does not extend the merged parts
    // that is, the extension has to be increased by at least some threshold

    int szExt;
    if( rowStart == szSeq1 )
    {
        // extension is suffix of the second
        szExt = szSeq2 - colStart;
    }
    else
    {
        szExt = szSeq1 - rowStart;
    }
    if( szExt < 0 )
    {
        THROW( "szExt: cannot be negative.");
    }
    if( szExt < MIN_ASM_EXT_LEN)
    {
        return OVERLAP_SMALLER_MINLENSCAFFOLD;
    }
#endif
    
    double scoreMinThres = szOverlap*(1-fractionLossScore);
//cout << "ScoreMinThres: " << scoreMinThres << ", scoreMax: " << scoreMax << endl;
    if(scoreMax < scoreMinThres)
		return OVERLAP_SMALLER_MINLENSCAFFOLD;

	
	//const int OVERLAP_SMALLER_MINLENSCAFFOLD=0;
	//const int OVERLAP_IN_RANGE=1;
	//const int OVERLAP_LARGER_MINLEN=2;

//if (szOverlap < minOverlapLenWithScaffold) cout << "OVERLAP_SMALLER_MINLENSCAFFOLD" << endl;
//else if (szOverlap >= minOverlapLenWithScaffold && szOverlap < minOverlapLen) cout << " OVERLAP_IN_RANGE" << endl;
//else cout<<" OVERLAP_LARGER_MINLEN"<<endl;

	if (szOverlap < minOverlapLenWithScaffold ) return OVERLAP_SMALLER_MINLENSCAFFOLD;
	else if(szOverlap>=minOverlapLenWithScaffold && szOverlap <minOverlapLen) return OVERLAP_IN_RANGE;
	else return OVERLAP_LARGER_MINLEN;

}


// ******************************************************************
// Quick checker of two sequences can have a chance to merge

QuickCheckerContigsMatch :: QuickCheckerContigsMatch() : pRepeatSeq(NULL), kmerLen(0)
{
}

QuickCheckerContigsMatch :: QuickCheckerContigsMatch(const QuickCheckerContigsMatch &rhs) : pRepeatSeq(rhs.pRepeatSeq), kmerLen( rhs.kmerLen), mapKmerFreqInRepeat(rhs.mapKmerFreqInRepeat)
{
    //
}

QuickCheckerContigsMatch :: QuickCheckerContigsMatch( FastaSequence *pRepeatSeqIn, int kmerLenIn ) : pRepeatSeq(pRepeatSeqIn),  kmerLen(kmerLenIn)
{
    //
    Init();
}

bool QuickCheckerContigsMatch :: IsMatchFeasible( FastaSequence *pScaffoldSeq ) const
{
    return IsMatchFeasibleV2(pScaffoldSeq);
#if 0
    //
    //
    const double minRatioKmerMatch = 0.2;

    vector<KmerTypeShort> listKmers1;
    GetKmersList( pScaffoldSeq, 0, pScaffoldSeq->size(), listKmers1 );
    
    
    // for now, we impose really weak condition: only a single k-mer matches (on both sides) we consider a plausible hit
    if( AreKmersMatch( listKmers1, minRatioKmerMatch ) == false  )
    {
        return false;
    }
    
    // since both ends seems to be fine
    return true;
#endif
}

bool QuickCheckerContigsMatch :: IsMatchFeasibleV2( FastaSequence *pScaffoldSeq ) const
{
    // we take k-mers from both ends of the new sequences; at least one should match with the existing k-mers 
    const double minRatioKmerMatch = 0.2;
    const int lenContigLen = 30;
    
    vector<KmerTypeShort> listKmers1;
    GetKmersList( pScaffoldSeq, 0, lenContigLen, listKmers1 );
    vector<KmerTypeShort> listKmers2;
    GetKmersList( pScaffoldSeq, pScaffoldSeq->size()-lenContigLen, lenContigLen, listKmers2 );
    

    // for now, we impose really weak condition: only a single k-mer matches (on both sides) we consider a plausible hit
    if( AreKmersMatch( listKmers1, minRatioKmerMatch ) == false && AreKmersMatch( listKmers2, minRatioKmerMatch ) == false  )
    {
        return false;
    }
    
    return true;   // for now
}

void QuickCheckerContigsMatch :: Init()
{
    //
    // fill in the kmer
    vector<KmerTypeShort> listKmers;
    GetKmersList(pRepeatSeq, 0, pRepeatSeq->size(), listKmers );
    for(int i=0; i<(int)listKmers.size(); ++i)
    {
        if( mapKmerFreqInRepeat.find( listKmers[i] ) == mapKmerFreqInRepeat.end() )
        {
            mapKmerFreqInRepeat.insert( map<KmerTypeShort,int> :: value_type(listKmers[i], 0) );
        }
        ++mapKmerFreqInRepeat[listKmers[i] ];
    }

}

void QuickCheckerContigsMatch :: GetKmersList( FastaSequence *pSeq, int posStart, int lenSeq, vector<KmerTypeShort> &listKmers ) const
{
    if( kmerLen > lenSeq)
    {
        cout << "FATAL ERROR: k-mer length is too large.\n";
        exit(1);
    }
    
    //
    const char *ptCharBuf = pSeq->c_str(posStart);
    GetAllKmersFromSeq( ptCharBuf, lenSeq, kmerLen, listKmers );
}


bool QuickCheckerContigsMatch :: AreKmersMatch( const vector<KmerTypeShort> &listKmers, double minRatioKmerMatch ) const
{
    // minRatioKmerMatch: fraction of kmers shared with repeats
    int numFound = 0;
    for(int i=0; i<(int)listKmers.size(); ++i )
    {
        if( mapKmerFreqInRepeat.find(  listKmers[i] ) != mapKmerFreqInRepeat.end() )
        {
            //
            ++numFound;
            break;
        }
    }
    //cout << "-- Number of k-mer matched: " << numFound << " out of " << listKmers.size() << " total kmers\n";
    // consider a good hit if one kmer is found
    if( numFound > 0 )
    {
        return true;
    }
    else
    {
        return false;
    }
}

