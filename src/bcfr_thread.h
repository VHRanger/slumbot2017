#ifndef _BCFR_THREAD_H_
#define _BCFR_THREAD_H_

#include <vector>

#include "vcfr.h"

using namespace std;

class BettingTree;
class Buckets;
class CFRConfig;
class HandTree;
class Node;

class BCFRThread : public VCFR {
public:
  BCFRThread(const CardAbstraction &ca, const BettingAbstraction &ba,
	     const CFRConfig &cc, const Buckets &buckets,
	     const BettingTree *betting_tree, unsigned int p,
	     HandTree *trunk_hand_tree, unsigned int thread_index,
	     unsigned int num_threads, unsigned int it, BCFRThread **threads,
	     bool trunk);
  ~BCFRThread(void);
  void Go(void);
  void AfterSplit(void);

  // Not supported yet
  static const unsigned int kSplitStreet = 999;
private:
  
  void WriteValues(Node *node, unsigned int gbd, const string &action_sequence,
		   double *vals);
  double *OurChoice(Node *node, unsigned int lbd, double *opp_probs,
		    double sum_opp_probs, double *total_card_probs,
		    unsigned int **street_buckets,
		    const string &action_sequence);
  double *OppChoice(Node *node, unsigned int lbd, double *opp_probs,
		    double sum_opp_probs, double *total_card_probs,
		    unsigned int **street_buckets,
		    const string &action_sequence);

  HandTree *trunk_hand_tree_;
  unsigned int thread_index_;
  BCFRThread **threads_;
  Node *split_node_;
  unsigned int split_bd_;
};

#endif
