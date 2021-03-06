#ifndef _VCFR_H_
#define _VCFR_H_

#include <semaphore.h>

#include <memory>
#include <string>

using namespace std;

class BettingAbstraction;
class BettingTree;
class Buckets;
class CanonicalCards;
class CardAbstraction;
class CFRConfig;
class CFRValues;
class HandTree;
class Node;
class VCFRSubgame;

class VCFR {
 public:
  VCFR(const CardAbstraction &ca, const BettingAbstraction &ba,
       const CFRConfig &cc, const Buckets &buckets,
       const BettingTree *betting_tree, unsigned int num_threads);
  virtual ~VCFR(void);
  virtual double *Process(Node *node, unsigned int lbd, double *opp_probs,
			  double sum_opp_probs, double *total_card_probs,
			  unsigned int **street_buckets,
			  const string &action_sequence, unsigned int last_st);
  unsigned int **InitializeStreetBuckets(void);
  void DeleteStreetBuckets(unsigned int **street_buckets);
  virtual void SetStreetBuckets(unsigned int st, unsigned int gbd,
				unsigned int **street_buckets);
  void SetIt(unsigned int it) {it_ = it;}
  void SetLastCheckpointIt(unsigned int it) {last_checkpoint_it_ = it;}
  void SetP(unsigned int p) {p_ = p;}
  void SetTargetP(unsigned int p) {target_p_ = p;}
  void SetBestResponseStreets(bool *sts);
  void SetBRCurrent(bool b) {br_current_ = b;}
  void SetValueCalculation(bool b) {value_calculation_ = b;}
  virtual void Post(unsigned int t);
  HandTree *GetHandTree(void) const {return hand_tree_;}
 protected:
  const unsigned int kMaxDepth = 100;
  
  virtual void UpdateRegrets(Node *node, double *vals, double **succ_vals,
			     int *regrets);
  virtual void UpdateRegrets(Node *node, double *vals, double **succ_vals,
			     double *regrets);
  virtual void UpdateRegretsBucketed(Node *node, unsigned int **street_buckets,
				     double *vals, double **succ_vals,
				     int *regrets);
  virtual void UpdateRegretsBucketed(Node *node, unsigned int **street_buckets,
				     double *vals, double **succ_vals,
				     double *regrets);
  virtual double *OurChoice(Node *node, unsigned int lbd, double *opp_probs,
			    double sum_opp_probs, double *total_card_probs,
			    unsigned int **street_buckets,
			    const string &action_sequence);
  virtual double *OppChoice(Node *node, unsigned int lbd, double *opp_probs,
			    double sum_opp_probs, double *total_card_probs,
			    unsigned int **street_buckets,
			    const string &action_sequence);
  virtual double *StreetInitial(Node *node, unsigned int lbd,
				double *opp_probs,
				unsigned int **street_buckets,
				const string &action_sequence);
  virtual void WaitForFinalSubgames(void);
  virtual void SpawnSubgame(Node *node, unsigned int bd,
			    const string &action_sequence, double *opp_probs);
  virtual void SetCurrentStrategy(Node *node);

  const CardAbstraction &card_abstraction_;
  const BettingAbstraction &betting_abstraction_;
  const CFRConfig &cfr_config_;
  const Buckets &buckets_;
  const BettingTree *betting_tree_;
  unique_ptr<CFRValues> regrets_;
  unique_ptr<CFRValues> sumprobs_;
  unique_ptr<CFRValues> current_strategy_;
  bool subgame_;
  unsigned int root_bd_st_;
  unsigned int root_bd_;
  // best_response_ is true in run_rgbr, build_cbrs, build_prbrs
  // Whenever best_response_ is true, value_calculation_ is true
  bool *best_response_streets_;
  bool br_current_;
  // value_calculation_ is true in run_rgbr, build_cbrs, build_prbrs,
  // build_cfrs.
  bool value_calculation_;
  bool prune_;
  unsigned int use_avg_for_current_it_;
  bool always_call_preflop_;
  unsigned int target_p_;
  unsigned int num_players_;
  unsigned int subgame_street_;
  unsigned int split_street_;
  bool nn_regrets_;
  bool uniform_;
  unsigned int soft_warmup_;
  unsigned int hard_warmup_;
  double explore_;
  bool double_regrets_;
  bool double_sumprobs_;
  bool *compressed_streets_;
  bool *sumprob_streets_;
  int *regret_floors_;
  int *regret_ceilings_;
  double *regret_scaling_;
  double *sumprob_scaling_;
  unsigned int it_;
  unsigned int last_checkpoint_it_;
  unsigned int p_;
  HandTree *hand_tree_;
  bool bucketed_; // Does at least one street have buckets?
  bool pre_phase_;
  double ****final_vals_;
  bool *subgame_running_;
  pthread_t *pthread_ids_;
  VCFRSubgame **active_subgames_;
  sem_t available_;
  unsigned int num_threads_;
};

void DeleteOldFiles(const CardAbstraction &ca, const BettingAbstraction &ba,
		    const CFRConfig &cc, unsigned int it);

#endif
