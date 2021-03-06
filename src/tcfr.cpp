// Should initialize buckets outside for either all streets or all streets
// but river if using HVB.
//
// Targeted CFR.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // sleep()

#include <algorithm>
#include <string>

#include "betting_abstraction.h"
#include "betting_tree.h"
#include "board_tree.h"
#include "buckets.h"
#include "canonical_cards.h"
#include "card_abstraction.h"
#include "cfr_config.h"
#include "constants.h"
#include "files.h"
#include "game.h"
#include "hand_tree.h"
#include "hand_value_tree.h"
#include "io.h"
#include "nonterminal_ids.h"
#include "rand.h"
#include "regret_compression.h"
#include "split.h"
#include "tcfr.h"

using namespace std;

#define SUCCPTR(ptr) (ptr + 8 + num_players_ * 4)

TCFRThread::TCFRThread(const BettingAbstraction &ba, const CFRConfig &cc,
		       const Buckets &buckets, unsigned int batch_index,
		       unsigned int num_threads, unsigned char *data,
		       unsigned int target_player, float *rngs,
		       unsigned int *uncompress, unsigned int *short_uncompress,
		       unsigned int *pruning_thresholds, bool *sumprob_streets,
		       unsigned char *hvb_table,
		       unsigned char ***cards_to_indices,
		       unsigned int batch_size) :
  betting_abstraction_(ba), cfr_config_(cc), buckets_(buckets) {
  batch_index_ = batch_index;
  num_threads_ = num_threads;
  data_ = data;
  asymmetric_ = betting_abstraction_.Asymmetric();
  num_players_ = Game::NumPlayers();
  target_player_ = target_player;
  rngs_ = rngs;
  rng_index_ = RandZeroToOne() * kNumPregenRNGs;
  uncompress_ = uncompress;
  short_uncompress_ = short_uncompress;
  pruning_thresholds_ = pruning_thresholds;
  sumprob_streets_ = sumprob_streets;
  hvb_table_ = hvb_table;
  cards_to_indices_ = cards_to_indices;
  batch_size_ = batch_size;
  
  max_street_ = Game::MaxStreet();
  quantized_streets_ = new bool[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    quantized_streets_[st] = false;
  }
  const vector<unsigned int> &qsv = cfr_config_.QuantizedStreets();
  unsigned int num_qsv = qsv.size();
  for (unsigned int i = 0; i < num_qsv; ++i) {
    unsigned int st = qsv[i];
    quantized_streets_[st] = true;
  }
  short_quantized_streets_ = new bool[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    short_quantized_streets_[st] = false;
  }
  const vector<unsigned int> &sqsv = cfr_config_.ShortQuantizedStreets();
  unsigned int num_sqsv = sqsv.size();
  for (unsigned int i = 0; i < num_sqsv; ++i) {
    unsigned int st = sqsv[i];
    short_quantized_streets_[st] = true;
  }
  scaled_streets_ = new bool[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    scaled_streets_[st] = false;
  }
  const vector<unsigned int> &ssv = cfr_config_.ScaledStreets();
  unsigned int num_ssv = ssv.size();
  for (unsigned int i = 0; i < num_ssv; ++i) {
    unsigned int st = ssv[i];
    scaled_streets_[st] = true;
  }
  explore_ = cfr_config_.Explore();
  full_only_avg_update_ = true; // cfr_config_.FullOnlyAvgUpdate();
  canon_bds_ = new unsigned int[max_street_ + 1];
  canon_bds_[0] = 0;
  hi_cards_ = new unsigned int[num_players_];
  lo_cards_ = new unsigned int[num_players_];
  hole_cards_ = new unsigned int[num_players_ * 2];
  hvs_ = new unsigned int[num_players_];
  hand_buckets_ = new unsigned int[num_players_ * (max_street_ + 1)];
  winners_ = new unsigned int[num_players_];
  succ_value_stack_ = new T_VALUE *[kStackDepth];
  succ_iregret_stack_ = new int *[kStackDepth];
  for (unsigned int i = 0; i < kStackDepth; ++i) {
    succ_value_stack_[i] = new T_VALUE[kMaxSuccs];
    succ_iregret_stack_[i] = new int[kMaxSuccs];
  }
  if (cfr_config_.NNR()) {
    fprintf(stderr, "NNR not supported\n");
    exit(-1);
  }
  const vector<int> &fv = cfr_config_.RegretFloors();
  if (fv.size() > 0) {
    fprintf(stderr, "Regret floors not supported\n");
    exit(-1);
  }
  sumprob_ceilings_ = new unsigned int[max_street_ + 1];
  const vector<unsigned int> &cv = cfr_config_.SumprobCeilings();
  if (cv.size() == 0) {
    for (unsigned int s = 0; s <= max_street_; ++s) {
      // Allow a little headroom to avoid overflow
      sumprob_ceilings_[s] = 4000000000U;
    }
  } else {
    if (cv.size() != max_street_ + 1) {
      fprintf(stderr, "Sumprob ceiling vector wrong size\n");
      exit(-1);
    }
    for (unsigned int s = 0; s <= max_street_; ++s) {
      sumprob_ceilings_[s] = cv[s];
    }
  }

  if (hvb_table_) {
    bytes_per_hand_ = 4ULL;
    if (buckets_.NumBuckets(max_street_) <= 65536) bytes_per_hand_ += 2;
    else                                           bytes_per_hand_ += 4;
  } else {
    bytes_per_hand_ = 0ULL;
  }

  full_ = new bool[max_street_ + 1];
  close_threshold_ = cfr_config_.CloseThreshold();

  active_mod_ = cfr_config_.ActiveMod();
  if (active_mod_ == 0) {
    fprintf(stderr, "Must set ActiveMod\n");
    exit(-1);
  }
  num_active_conditions_ = cfr_config_.NumActiveConditions();
  if (num_active_conditions_ > 0) {
    num_active_streets_ = new unsigned int[num_active_conditions_];
    num_active_rems_ = new unsigned int[num_active_conditions_];
    active_streets_ = new unsigned int *[num_active_conditions_];
    active_rems_ = new unsigned int *[num_active_conditions_];
    for (unsigned int c = 0; c < num_active_conditions_; ++c) {
      num_active_streets_[c] = cfr_config_.NumActiveStreets(c);
      num_active_rems_[c] = cfr_config_.NumActiveRems(c);
      active_streets_[c] = new unsigned int[num_active_streets_[c]];
      for (unsigned int i = 0; i < num_active_streets_[c]; ++i) {
	active_streets_[c][i] = cfr_config_.ActiveStreet(c, i);
      }
      active_rems_[c] = new unsigned int[num_active_rems_[c]];
      for (unsigned int i = 0; i < num_active_rems_[c]; ++i) {
	active_rems_[c][i] = cfr_config_.ActiveRem(c, i);
      }
    }
  } else {
    num_active_streets_ = NULL;
    num_active_rems_ = NULL;
    active_streets_ = NULL;
    active_rems_ = NULL;
  }

  srand48_r(batch_index_, &rand_buf_);
}

TCFRThread::~TCFRThread(void) {
  for (unsigned int c = 0; c < num_active_conditions_; ++c) {
    delete [] active_streets_[c];
    delete [] active_rems_[c];
  }
  delete [] num_active_streets_;
  delete [] num_active_rems_;
  delete [] active_streets_;
  delete [] active_rems_;
  delete [] full_;
  delete [] sumprob_ceilings_;
  delete [] quantized_streets_;
  delete [] short_quantized_streets_;
  delete [] scaled_streets_;
  for (unsigned int i = 0; i < kStackDepth; ++i) {
    delete [] succ_value_stack_[i];
    delete [] succ_iregret_stack_[i];
  }
  delete [] succ_value_stack_;
  delete [] succ_iregret_stack_;
  delete [] hand_buckets_;
  delete [] canon_bds_;
  delete [] hi_cards_;
  delete [] lo_cards_;
  delete [] hole_cards_;
  delete [] hvs_;
  delete [] winners_;
}

bool TCFRThread::HVBDealHand(void) {
  // For holdem5/mb2b1, 1b its takes about 71m.
  if (it_ == batch_size_ + 1) return false;
  unsigned int num_boards = BoardTree::NumBoards(max_street_);
  double r;
  drand48_r(&rand_buf_, &r);
  unsigned int msbd = r * num_boards;
  canon_bds_[max_street_] = msbd;
  board_count_ = BoardTree::BoardCount(max_street_, msbd);
  for (unsigned int st = 1; st < max_street_; ++st) {
    canon_bds_[st] = BoardTree::PredBoard(msbd, st);
  }
  const Card *board = BoardTree::Board(max_street_, msbd);
  unsigned int num_ms_board_cards = Game::NumBoardCards(max_street_);
  unsigned int end_cards = Game::MaxCard() + 1;

  for (unsigned int p = 0; p < num_players_; ++p) {
    unsigned int c1, c2;
    while (true) {
      drand48_r(&rand_buf_, &r);
      c1 = end_cards * r;
      if (InCards(c1, board, num_ms_board_cards)) continue;
      if (InCards(c1, hole_cards_, 2 * p)) continue;
      break;
    }
    hole_cards_[2 * p] = c1;
    while (true) {
      drand48_r(&rand_buf_, &r);
      c2 = end_cards * r;
      if (InCards(c2, board, num_ms_board_cards)) continue;
      if (InCards(c2, hole_cards_, 2 * p + 1)) continue;
      break;
    }
    hole_cards_[2 * p + 1] = c2;
    if (c1 > c2) {hi_cards_[p] = c1; lo_cards_[p] = c2;}
    else         {hi_cards_[p] = c2; lo_cards_[p] = c1;}
  }
  
  for (unsigned int p = 0; p < num_players_; ++p) hvs_[p] = 0;

  for (unsigned int st = 0; st <= max_street_; ++st) {
    unsigned int num_hole_card_pairs = Game::NumHoleCardPairs(st);
    unsigned int bd = canon_bds_[st];
    unsigned int base = bd * num_hole_card_pairs;
    for (unsigned int p = 0; p < num_players_; ++p) {
      unsigned char hi = cards_to_indices_[st][bd][hi_cards_[p]];
      unsigned char li = cards_to_indices_[st][bd][lo_cards_[p]];
      // The sum from 1... hi_index - 1 is the number of hole card pairs
      // containing a high card less than hi.
      unsigned int hcp = (hi - 1) * hi / 2 + li;
      unsigned int h = base + hcp;

      if (st == max_street_) {
	unsigned char *ptr = &hvb_table_[h * bytes_per_hand_];
	if (buckets_.NumBuckets(max_street_) <= 65536) {
	  hand_buckets_[p * (max_street_ + 1) + max_street_] =
	    *(unsigned short *)ptr;
	  ptr += 2;
	} else {
	  hand_buckets_[p * (max_street_ + 1) + max_street_] =
	    *(unsigned int *)ptr;
	  ptr += 4;
	}
	hvs_[p] = *(unsigned int *)ptr;
      } else {
	hand_buckets_[p * (max_street_ + 1) + st] = buckets_.Bucket(st, h);
      }
    }
  }

  return true;
}

// Our old implementation which is a bit slower.
bool TCFRThread::NoHVBDealHand(void) {
  // For holdem5/mb2b1, 1b its takes about 71m.
  if (it_ == batch_size_ + 1) return false;
  unsigned int num_boards = BoardTree::NumBoards(max_street_);
  double r;
  drand48_r(&rand_buf_, &r);
  unsigned int msbd = r * num_boards;
  canon_bds_[max_street_] = msbd;
  board_count_ = BoardTree::BoardCount(max_street_, msbd);
  if (board_count_ == 0) {
    fprintf(stderr, "it_ %llu msbd %u board_count_ %i\n", it_, msbd,
	    board_count_);
    fprintf(stderr, "Num boards: %u\n", BoardTree::NumBoards(max_street_));
    exit(-1);
  }
  for (unsigned int st = 1; st < max_street_; ++st) {
    canon_bds_[st] = BoardTree::PredBoard(msbd, st);
  }
  const Card *board = BoardTree::Board(max_street_, msbd);
  Card cards[7];
  unsigned int num_ms_board_cards = Game::NumBoardCards(max_street_);
  for (unsigned int i = 0; i < num_ms_board_cards; ++i) {
    cards[i+2] = board[i];
  }
  int end_cards = Game::MaxCard() + 1;

  for (unsigned int p = 0; p < num_players_; ++p) {
    unsigned int c1, c2;
    while (true) {
      drand48_r(&rand_buf_, &r);
      c1 = end_cards * r;
      if (InCards(c1, board, num_ms_board_cards)) continue;
      if (InCards(c1, hole_cards_, 2 * p)) continue;
      break;
    }
    hole_cards_[2 * p] = c1;
    while (true) {
      drand48_r(&rand_buf_, &r);
      c2 = end_cards * r;
      if (InCards(c2, board, num_ms_board_cards)) continue;
      if (InCards(c2, hole_cards_, 2 * p + 1)) continue;
      break;
    }
    hole_cards_[2 * p + 1] = c2;
    if (c1 > c2) {hi_cards_[p] = c1; lo_cards_[p] = c2;}
    else         {hi_cards_[p] = c2; lo_cards_[p] = c1;}
  }


  for (unsigned int p = 0; p < num_players_; ++p) hvs_[p] = 0;

  for (unsigned int st = 0; st <= max_street_; ++st) {
    unsigned int bd = canon_bds_[st];
    unsigned int num_hole_card_pairs = Game::NumHoleCardPairs(st);
    for (unsigned int p = 0; p < num_players_; ++p) {
      cards[0] = hi_cards_[p];
      cards[1] = lo_cards_[p];
      unsigned int hcp = HCPIndex(st, cards);
      unsigned int h = bd * num_hole_card_pairs + hcp;
      hand_buckets_[p * (max_street_ + 1) + st] = buckets_.Bucket(st, h);
      if (st == max_street_) {
	hvs_[p] = HandValueTree::Val(cards);
      }
    }
  }

  return true;
}

static double *g_preflop_vals = nullptr;
static unsigned long long int *g_preflop_nums = nullptr;

void TCFRThread::Run(void) {
  process_count_ = 0ULL;
  full_process_count_ = 0ULL;
  it_ = 1;
  unique_ptr<long long int []> sum_values(new long long int[num_players_]);
  unique_ptr<long long int []> denoms(new long long int[num_players_]);
  for (unsigned int p = 0; p < num_players_; ++p) {
    sum_values[p] = 0LL;
    denoms[p] = 0LL;
  }
  
  while (1) {
    bool not_done;
    if (hvb_table_) not_done = HVBDealHand();
    else            not_done = NoHVBDealHand();
    if (! not_done) break;

    if (it_ % 10000000 == 1 && batch_index_ % num_threads_ == 0) {
      fprintf(stderr, "Batch %i it %llu\n", batch_index_, it_);
    }

    all_full_ = false;

    if (! all_full_) {
      for (unsigned int st = 0; st <= max_street_; ++st) full_[st] = false;
      unsigned int rem = it_ % active_mod_;
      unsigned int c;
      for (c = 0; c < num_active_conditions_; ++c) {
	unsigned int num = num_active_rems_[c];
	for (unsigned int i = 0; i < num; ++i) {
	  unsigned int this_rem = active_rems_[c][i];
	  if (rem == this_rem) {
	    goto BREAKOUT;
	  }
	}
      }
    BREAKOUT:
      if (c == num_active_conditions_) {
	all_full_ = true;
      } else {
	unsigned int num = num_active_streets_[c];
	for (unsigned int i = 0; i < num; ++i) {
	  unsigned int st = active_streets_[c][i];
	  full_[st] = true;
	}
      }
    }

    for (p_ = 0; p_ < num_players_; ++p_) {
      stack_index_ = 0;
      T_VALUE val = Process(data_);
      sum_values[p_] += val;
      denoms[p_] += board_count_;
      unsigned int b = hand_buckets_[p_ * (max_street_ + 1)];
      g_preflop_vals[b] += val;
      g_preflop_nums[b] += board_count_;
    }

    ++it_;
    if (it_ % 10000000 == 0 && batch_index_ % num_threads_ == 0) {
      for (unsigned int p = 0; p < num_players_; ++p) {
	fprintf(stderr, "It %llu avg P%u val %f\n", it_, p,
		sum_values[p] / (double)denoms[p]);
      }
    }
  }
  fprintf(stderr, "Batch %i done\n", batch_index_);
  if (batch_index_ % num_threads_ == 0) {
    for (unsigned int p = 0; p < num_players_; ++p) {
      fprintf(stderr, "Batch %i avg P%u val %f\n", batch_index_, p,
	      sum_values[p] / (double)denoms[p]);
    }
  }
}

static void *thread_run(void *v_t) {
  TCFRThread *t = (TCFRThread *)v_t;
  t->Run();
  return NULL;
}

void TCFRThread::RunThread(void) {
  pthread_create(&pthread_id_, NULL, thread_run, this);
}

void TCFRThread::Join(void) {
  pthread_join(pthread_id_, NULL); 
}

int TCFRThread::Round(double d) {
  double rnd = rngs_[rng_index_++];
  if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
  if (d < 0) {
    int below = d;
    double rem = below - d;
    if (rnd < rem) {
      return below - 1;
    } else {
      return below;
    }
  } else {
    int below = d;
    double rem = d - below;
    if (rnd < rem) {
      return below + 1;
    } else {
      return below;
    }
  }
}

// This is unfinished.  We have nonterminal fold nodes now.  Need to
// return when current player folds.  Don't continue on until a terminal
// node.
T_VALUE TCFRThread::Process(unsigned char *ptr) {
  ++process_count_;
  if (all_full_) {
    ++full_process_count_;
  }
  unsigned char first_byte = ptr[0];
  if (first_byte > 1) {
    // Showdown

    // Find the best hand value of anyone remaining in the hand, and the
    // total pot size which includes contributions from remaining players
    // and players who folded earlier.
    unsigned int best_hv = 0;
    int pot_size = 0;
    for (unsigned int p = 0; p < num_players_; ++p) {
      pot_size += *(int *)(ptr + 8 + p * 4);
      // fprintf(stderr, "p %u contribution %i offset %llu\n", p,
      // *(int *)(ptr + 8 + p * 4),
      // (unsigned long long int)(ptr - data_));
      if (ptr[p+1] == 1) {
	unsigned int hv = hvs_[p];
	if (hv > best_hv) best_hv = hv;
      }
    }

    // Determine if we won, the number of winners, and the total contribution
    // of all winners.
    unsigned int num_winners = 0;
    int winner_contributions = 0;
    bool we_win = false;
    for (unsigned int p = 0; p < num_players_; ++p) {
      if (ptr[p+1] == 1 && hvs_[p] == best_hv) {
	winners_[num_winners++] = p;
	winner_contributions += *(int *)(ptr + 8 + p * 4);
	we_win |= (p == p_);
      }
    }

    int ret;
    if (we_win) {
      // Our winnings is:
      // a) The total pot
      // b) Minus the contributions of the winners
      // c) Divided by the number of winners
      double winnings =
	(((double)(pot_size - winner_contributions)) /
	 ((double)num_winners)) * board_count_;
      // Normally the winnings are a whole number, but not always.
      ret = Round(winnings);
#if 0
      fprintf(stderr, "Winnings %f ps %i bc %i\n", winnings, pot_size,
	      board_count_);
#endif
    } else {
	// If we lose at showdown, we lose the amount we contributed to the pot.
      ret = -*(int *)(ptr + 8 + p_ * 4) * board_count_;
      // fprintf(stderr, "Losses %i board_count_ %i\n", ret, board_count_);
    }
    if (ret <= -100000 || ret >= 100000) {
      fprintf(stderr, "OOB ret %i bc %i\n", ret, board_count_);
      exit(-1);
    }
    return ret;
  } else if (first_byte == 1) {
    // Fold
    // Assume if we get here that we are the winner.
    // pot_size is contributions of all players other than ourselves.
    int pot_size = 0;
    for (unsigned int p = 0; p < num_players_; ++p) {
      if (p != p_) {
	pot_size += *(int *)(ptr + 8 + p * 4);
      }
    }
    return pot_size * board_count_;
  } else { // Nonterminal node
    unsigned int num_succs = ptr[2];
    if (num_succs == 1) {
      unsigned long long int succ_offset =
	*((unsigned long long int *)(SUCCPTR(ptr)));
      return Process(data_ + succ_offset);
    }
    unsigned int st = ptr[1];
    unsigned int default_succ_index = 0;
    unsigned int fold_succ_index = ptr[3];
    unsigned int player_acting = *(unsigned int *)(ptr + 4);
    if (player_acting == p_) {
      unsigned int our_bucket = hand_buckets_[p_ * (max_street_ + 1) + st];

      unsigned int size_bucket_data;
      if (quantized_streets_[st]) {
	size_bucket_data = num_succs;
      } else if (short_quantized_streets_[st]) {
	size_bucket_data = num_succs * 2;
      } else {
	size_bucket_data = num_succs * sizeof(T_REGRET);
      }
      if (sumprob_streets_[st]) {
	if (! asymmetric_ || target_player_ == player_acting) {
	  size_bucket_data += num_succs * sizeof(T_SUM_PROB);
	}
      }
      unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
      ptr1 += our_bucket * size_bucket_data;
      // ptr1 has now skipped past prior buckets

      unsigned int min_s = kMaxUInt;
      // min_r2 is second best regret
      unsigned int min_r = kMaxUInt, min_r2 = kMaxUInt;

      if (quantized_streets_[st]) {
	unsigned char *bucket_regrets = ptr1;
	unsigned char min_qr = 255, min_qr2 = 255;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  // There should always be one action with regret 0
	  unsigned char qr = bucket_regrets[s];
	  if (qr < min_qr) {
	    min_s = s;
	    min_qr2 = min_qr;
	    min_qr = qr;
	  } else if (qr < min_qr2) {
	    min_qr2 = qr;
	  }
	}
	min_r = uncompress_[min_qr];
	min_r2 = uncompress_[min_qr2];
      } else if (short_quantized_streets_[st]) {
	unsigned short *bucket_regrets = (unsigned short *)ptr1;
	unsigned short min_qr = 65535, min_qr2 = 65535;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  // There should always be one action with regret 0
	  unsigned short qr = bucket_regrets[s];
	  if (qr < min_qr) {
	    min_s = s;
	    min_qr2 = min_qr;
	    min_qr = qr;
	  } else if (qr < min_qr2) {
	    min_qr2 = qr;
	  }
	}
	min_r = short_uncompress_[min_qr];
	min_r2 = short_uncompress_[min_qr2];
      } else {
	T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  // There should always be one action with regret 0
	  T_REGRET r = bucket_regrets[s];
	  if (r < min_r) {
	    min_s = s;
	    min_r2 = min_r;
	    min_r = r;
	  } else if (r < min_r2) {
	    min_r2 = r;
	  }
	}
      }

      bool recurse_on_all;
      if (all_full_) {
	recurse_on_all = true;
      } else {
	// Could consider only recursing on close children.
	bool close = ((min_r2 - min_r) < close_threshold_);
	recurse_on_all = full_[st] || close;
      }

      T_VALUE *succ_values = succ_value_stack_[stack_index_];
      unsigned int pruning_threshold = pruning_thresholds_[st];
      T_VALUE val;
      if (! recurse_on_all) {
	unsigned int s = min_s;
	if (explore_ > 0) {
	  double thresh = explore_ * num_succs;
	  double rnd = rngs_[rng_index_++];
	  if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
	  if (rnd < thresh) {
	    s = rnd / explore_;
	  }
	}
	if (s == fold_succ_index) {
	  int contrib = *((int *)(ptr + 8 + p_ * 4));
	  val = -contrib * board_count_;
	} else {
	  unsigned long long int succ_offset =
	    *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
	  val = Process(data_ + succ_offset);
	}
      } else { // Recursing on all succs
	for (unsigned int s = 0; s < num_succs; ++s) {
	  unsigned long long int succ_offset =
	    *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
	  
	  bool prune = false;
	  if (! quantized_streets_[st] && ! short_quantized_streets_[st]) {
	    T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	    prune = (bucket_regrets[s] >= pruning_threshold);
	  }
	  if (s == fold_succ_index || ! prune) {
	    if (s == fold_succ_index) {
	      int contrib = *((int *)(ptr + 8 + p_ * 4));
	      succ_values[s] = -contrib * board_count_;
	    } else {
	      ++stack_index_;
	      succ_values[s] = Process(data_ + succ_offset);
	      --stack_index_;
	    }
	  }
	}
	  
	val = succ_values[min_s];

	int *succ_iregrets = succ_iregret_stack_[stack_index_];
	int min_regret = kMaxInt;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  int ucr;
	  if (quantized_streets_[st]) {
	    unsigned char *bucket_regrets = ptr1;
	    ucr = uncompress_[bucket_regrets[s]];
	  } else if (short_quantized_streets_[st]) {
	    unsigned short *bucket_regrets = (unsigned short *)ptr1;
	    ucr = short_uncompress_[bucket_regrets[s]];
	  } else {
	    T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	    if (s != fold_succ_index &&
		bucket_regrets[s] >= pruning_threshold) {
	      continue;
	    }
	    ucr = bucket_regrets[s];
	  }
	  int i_regret;
	  if (scaled_streets_[st]) {
	    int incr = succ_values[s] - val;
	    double scaled = incr * 0.05;
	    int trunc = scaled;
	    double rnd = rngs_[rng_index_++];
	    if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
	    if (scaled < 0) {
	      double rem = trunc - scaled;
	      if (rnd < rem) incr = trunc - 1;
	      else           incr = trunc;
	    } else {
	      double rem = scaled - trunc;
	      if (rnd < rem) incr = trunc + 1;
	      else           incr = trunc;
	    }
	    i_regret = ucr - incr;
	  } else {
	    i_regret = ucr - (succ_values[s] - val);
	  }
	  if (s == 0 || i_regret < min_regret) min_regret = i_regret;
	  succ_iregrets[s] = i_regret;
	}
	int offset = -min_regret;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  // Assume no pruning if quantization for now
	  if (! quantized_streets_[st] && ! short_quantized_streets_[st]) {
	    T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	    if (s != fold_succ_index &&
		bucket_regrets[s] >= pruning_threshold) {
	      continue;
	    }
	  }
	  int i_regret = succ_iregrets[s];
	  unsigned int r = (unsigned int)(i_regret + offset);
	  if (quantized_streets_[st]) {
	    unsigned char *bucket_regrets = ptr1;
	    double rnd = rngs_[rng_index_++];
	    if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
	    bucket_regrets[s] = CompressRegret(r, rnd, uncompress_);
	  } else if (short_quantized_streets_[st]) {
	    unsigned short *bucket_regrets = (unsigned short *)ptr1;
	    double rnd = rngs_[rng_index_++];
	    if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
	    bucket_regrets[s] = CompressRegretShort(r, rnd, short_uncompress_);
	  } else {
	    T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	    // Try capping instead of dividing by two.  Make sure to apply
	    // cap after adding offset.
	    if (r > 2000000000) r = 2000000000;
	    bucket_regrets[s] = r;
	  }
	}
      }
      return val;
    } else {
      unsigned int opp_bucket =
	hand_buckets_[player_acting * (max_street_ + 1) + st];

      unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
      unsigned int size_bucket_data;
      if (quantized_streets_[st]) {
	size_bucket_data = num_succs;
      } else if (short_quantized_streets_[st]) {
	size_bucket_data = num_succs * 2;
      } else {
	size_bucket_data = num_succs * sizeof(T_REGRET);
      }
      if (sumprob_streets_[st]) {
	if (! asymmetric_ || target_player_ == player_acting) {
	  size_bucket_data += num_succs * sizeof(T_SUM_PROB);
	}
      }

      ptr1 += opp_bucket * size_bucket_data;
      // ptr1 has now skipped past prior buckets

      unsigned int min_s = default_succ_index;
      // There should always be one action with regret 0
      if (quantized_streets_[st]) {
	unsigned char *bucket_regrets = ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  if (bucket_regrets[s] == 0) {
	    min_s = s;
	    break;
	  }
	}
      } else if (short_quantized_streets_[st]) {
	unsigned short *bucket_regrets = (unsigned short *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  if (bucket_regrets[s] == 0) {
	    min_s = s;
	    break;
	  }
	}
      } else {
	T_REGRET *bucket_regrets = (T_REGRET *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  if (bucket_regrets[s] == 0) {
	    min_s = s;
	    break;
	  }
	}
      }

      unsigned int ss = min_s;
      if (explore_ > 0) {
	double thresh = explore_ * num_succs;
	double rnd = rngs_[rng_index_++];
	if (rng_index_ == kNumPregenRNGs) rng_index_ = 0;
	if (rnd < thresh) {
	  ss = rnd / explore_;
	}
      }

	// Update sum-probs
      if (sumprob_streets_[st] && (all_full_ || ! full_only_avg_update_)) {
	if (! asymmetric_ || player_acting == target_player_) {
	  T_SUM_PROB *these_sum_probs;
	  if (quantized_streets_[st]) {
	    these_sum_probs = (T_SUM_PROB *)(ptr1 + num_succs);
	  } else if (short_quantized_streets_[st]) {
	    these_sum_probs = (T_SUM_PROB *)(ptr1 + num_succs * 2);
	  } else {
	    these_sum_probs =
	      (T_SUM_PROB *)(ptr1 + num_succs * sizeof(T_REGRET));
	  }
	  T_SUM_PROB ceiling = sumprob_ceilings_[st];
	  these_sum_probs[ss] += 1;
	  bool sum_prob_too_extreme = false;
	  if (these_sum_probs[ss] > ceiling) {
	    sum_prob_too_extreme = true;
	  }
	  if (sum_prob_too_extreme) {
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      these_sum_probs[s] /= 2;
	    }
	  }
	}
      }

      unsigned long long int succ_offset =
	*((unsigned long long int *)(SUCCPTR(ptr) + ss * 8));
      ++stack_index_;
      T_VALUE ret = Process(data_ + succ_offset);
      --stack_index_;
      return ret;
    }
  }
}

void TCFR::ReadRegrets(unsigned char *ptr, Node *node, Reader ***readers,
		       bool ***seen) {
  unsigned char first_byte = ptr[0];
  // Terminal node
  if (first_byte != 0) return;
  unsigned int num_succs = ptr[2];
  if (num_succs > 1) {
    unsigned int pa = *(unsigned int *)(ptr + 4);
    unsigned int st = ptr[1];
    unsigned int nt = node->NonterminalID();
    if (seen[st][pa][nt]) return;
    seen[st][pa][nt] = true;
    Reader *reader = readers[pa][st];
    unsigned int num_buckets = buckets_.NumBuckets(st);
    unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
    if (quantized_streets_[st]) {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	for (unsigned int s = 0; s < num_succs; ++s) {
	  ptr1[s] = reader->ReadUnsignedCharOrDie();
	}
	ptr1 += num_succs;
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    } else if (short_quantized_streets_[st]) {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	unsigned short *regrets = (unsigned short *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  regrets[s] = reader->ReadUnsignedShortOrDie();
	}
	ptr1 += num_succs * 2;
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    } else {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	T_REGRET *regrets = (T_REGRET *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  regrets[s] = reader->ReadUnsignedIntOrDie();
	}
	ptr1 += num_succs * sizeof(T_REGRET);
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    }
  }
  for (unsigned int s = 0; s < num_succs; ++s) {
    unsigned long long int succ_offset =
      *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
    ReadRegrets(data_ + succ_offset, node->IthSucc(s), readers, seen);
  }
}

void TCFR::WriteRegrets(unsigned char *ptr, Node *node, Writer ***writers,
			bool ***seen) {
  unsigned char first_byte = ptr[0];
  // Terminal node
  if (first_byte != 0) return;
  unsigned int num_succs = ptr[2];
  if (num_succs > 1) {
    unsigned int pa = *(unsigned int *)(ptr + 4);
    unsigned int st = ptr[1];
    unsigned int nt = node->NonterminalID();
    if (seen[st][pa][nt]) return;
    seen[st][pa][nt] = true;
    Writer *writer = writers[pa][st];
    unsigned int num_buckets = buckets_.NumBuckets(st);
    unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
    if (quantized_streets_[st]) {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	for (unsigned int s = 0; s < num_succs; ++s) {
	  writer->WriteUnsignedChar(ptr1[s]);
	}
	ptr1 += num_succs;
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    } else if (short_quantized_streets_[st]) {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	unsigned short *regrets = (unsigned short *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  writer->WriteUnsignedShort(regrets[s]);
	}
	ptr1 += num_succs * 2;
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    } else {
      for (unsigned int b = 0; b < num_buckets; ++b) {
	T_REGRET *regrets = (T_REGRET *)ptr1;
	for (unsigned int s = 0; s < num_succs; ++s) {
	  writer->WriteUnsignedInt(regrets[s]);
	}
	ptr1 += num_succs * sizeof(T_REGRET);
	if (sumprob_streets_[st]) {
	  if (! asymmetric_ || target_player_ == pa) {
	    ptr1 += num_succs * sizeof(T_SUM_PROB);
	  }
	}
      }
    }
  }
  for (unsigned int s = 0; s < num_succs; ++s) {
    unsigned long long int succ_offset =
      *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
    WriteRegrets(data_ + succ_offset, node->IthSucc(s), writers, seen);
  }
}

void TCFR::ReadSumprobs(unsigned char *ptr, Node *node, Reader ***readers,
			bool ***seen) {
  unsigned char first_byte = ptr[0];
  // Terminal node
  if (first_byte != 0) return;
  unsigned int num_succs = ptr[2];
  if (num_succs > 1) {
    unsigned int pa = *(unsigned int *)(ptr + 4);
    unsigned int st = ptr[1];
    unsigned int nt = node->NonterminalID();
    if (seen[st][pa][nt]) return;
    seen[st][pa][nt] = true;
    unsigned int num_buckets = buckets_.NumBuckets(st);
    unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
    if (sumprob_streets_[st]) {
      if (! asymmetric_ || target_player_ == pa) {
	Reader *reader = readers[pa][st];
	if (quantized_streets_[st]) {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs = (T_SUM_PROB *)(ptr1 + num_succs);
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      sum_probs[s] = reader->ReadUnsignedIntOrDie();
	    }
	    ptr1 += num_succs * (1 + sizeof(T_SUM_PROB));
	  }
	} else if (short_quantized_streets_[st]) {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs = (T_SUM_PROB *)(ptr1 + num_succs * 2);
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      sum_probs[s] = reader->ReadUnsignedIntOrDie();
	    }
	    ptr1 += num_succs * (2 + sizeof(T_SUM_PROB));
	  }
	} else {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs =
	      (T_SUM_PROB *)(ptr1 + num_succs * sizeof(T_REGRET));
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      sum_probs[s] = reader->ReadUnsignedIntOrDie();
	    }
	    ptr1 += num_succs * (sizeof(T_REGRET) + sizeof(T_SUM_PROB));
	  }
	}
      }
    }
  }
  for (unsigned int s = 0; s < num_succs; ++s) {
    unsigned long long int succ_offset =
      *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
    ReadSumprobs(data_ + succ_offset, node->IthSucc(s), readers, seen);
  }
}

void TCFR::WriteSumprobs(unsigned char *ptr, Node *node, Writer ***writers,
			 bool ***seen) {
  unsigned char first_byte = ptr[0];
  // Terminal node
  if (first_byte != 0) return;
  unsigned int num_succs = ptr[2];
  if (num_succs > 1) {
    unsigned int pa = *(unsigned int *)(ptr + 4);
    unsigned int st = ptr[1];
    unsigned int nt = node->NonterminalID();
    if (seen[st][pa][nt]) return;
    seen[st][pa][nt] = true;
    if (sumprob_streets_[st]) {
      if (! asymmetric_ || target_player_ == pa) {
	Writer *writer = writers[pa][st];
	unsigned int num_buckets = buckets_.NumBuckets(st);
	unsigned char *ptr1 = SUCCPTR(ptr) + num_succs * 8;
	if (quantized_streets_[st]) {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs = (T_SUM_PROB *)(ptr1 + num_succs);
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      writer->WriteUnsignedInt(sum_probs[s]);
	    }
	    ptr1 += num_succs * (1 + sizeof(T_SUM_PROB));
	  }
	} else if (short_quantized_streets_[st]) {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs = (T_SUM_PROB *)(ptr1 + num_succs * 2);
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      writer->WriteUnsignedInt(sum_probs[s]);
	    }
	    ptr1 += num_succs * (2 + sizeof(T_SUM_PROB));
	  }
	} else {
	  for (unsigned int b = 0; b < num_buckets; ++b) {
	    T_SUM_PROB *sum_probs =
	      (T_SUM_PROB *)(ptr1 + num_succs * sizeof(T_REGRET));
	    for (unsigned int s = 0; s < num_succs; ++s) {
	      writer->WriteUnsignedInt(sum_probs[s]);
	    }
	    ptr1 += num_succs * (sizeof(T_REGRET) + sizeof(T_SUM_PROB));
	  }
	}
      }
    }
  }
  for (unsigned int s = 0; s < num_succs; ++s) {
    unsigned long long int succ_offset =
      *((unsigned long long int *)(SUCCPTR(ptr) + s * 8));
    WriteSumprobs(data_ + succ_offset, node->IthSucc(s), writers, seen);
  }
}

void TCFR::Read(unsigned int batch_base) {
  char dir[500], buf[500];
  sprintf(dir, "%s/%s.%u.%s.%i.%i.%i.%s.%s", Files::OldCFRBase(),
	  Game::GameName().c_str(), Game::NumPlayers(),
	  card_abstraction_.CardAbstractionName().c_str(), Game::NumRanks(),
	  Game::NumSuits(), Game::MaxStreet(), 
	  betting_abstraction_.BettingAbstractionName().c_str(),
	  cfr_config_.CFRConfigName().c_str());
  if (asymmetric_) {
    char buf2[20];
    sprintf(buf2, ".p%u", target_player_);
    strcat(dir, buf2);
  }
  unsigned int num_players = Game::NumPlayers();
  Reader ***regret_readers = new Reader **[num_players];
  for (unsigned int p = 0; p < num_players; ++p) {
    regret_readers[p] = new Reader *[max_street_ + 1];
    for (unsigned int st = 0; st <= max_street_; ++st) {
      sprintf(buf, "%s/regrets.x.0.0.%u.%u.p%u.i", dir,
	      st, batch_base, p);
      regret_readers[p][st] = new Reader(buf);
    }
  }
  Reader ***sum_prob_readers = new Reader **[num_players];
  for (unsigned int p = 0; p < num_players; ++p) {
    if (asymmetric_ && target_player_ != p) {
      sum_prob_readers[p] = NULL;
      continue;
    }
    sum_prob_readers[p] = new Reader *[max_street_ + 1];
    for (unsigned int st = 0; st <= max_street_; ++st) {
      if (! sumprob_streets_[st]) {
	sum_prob_readers[p][st] = NULL;
	continue;
      }
      if (! asymmetric_ || target_player_ == p) {
	sprintf(buf, "%s/sumprobs.x.0.0.%u.%u.p%u.i", dir,
		st, batch_base, p);
	sum_prob_readers[p][st] = new Reader(buf);
      } else {
	sum_prob_readers[p][st] = nullptr;
      }
    }
  }
  bool ***seen = new bool **[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    seen[st] = new bool *[num_players];
    for (unsigned int p = 0; p < num_players; ++p) {
      unsigned int num_nt = betting_tree_->NumNonterminals(p, st);
      seen[st][p] = new bool[num_nt];
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][p][i] = false;
      }
    }
  }
  ReadRegrets(data_, betting_tree_->Root(), regret_readers, seen);
  for (unsigned int st = 0; st <= max_street_; ++st) {
    for (unsigned int p = 0; p < num_players; ++p) {
      unsigned int num_nt = betting_tree_->NumNonterminals(p, st);
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][p][i] = false;
      }
    }
  }
  ReadSumprobs(data_, betting_tree_->Root(), sum_prob_readers, seen);
  for (unsigned int st = 0; st <= max_street_; ++st) {
    for (unsigned int p = 0; p < num_players; ++p) {
      delete [] seen[st][p];
    }
    delete [] seen[st];
  }
  delete [] seen;
  for (unsigned int p = 0; p <= 1; ++p) {
    for (unsigned int st = 0; st <= max_street_; ++st) {
      if (! regret_readers[p][st]->AtEnd()) {
	fprintf(stderr, "Regret reader didn't get to EOF\n");
	exit(-1);
      }
      delete regret_readers[p][st];
    }
    delete [] regret_readers[p];
  }
  delete [] regret_readers;
  for (unsigned int p = 0; p <= 1; ++p) {
    if (asymmetric_ && target_player_ != p) continue;
    for (unsigned int st = 0; st <= max_street_; ++st) {
      if (! sumprob_streets_[st]) continue;
      if (! sum_prob_readers[p][st]->AtEnd()) {
	fprintf(stderr, "Sumprob reader didn't get to EOF\n");
	exit(-1);
      }
      delete sum_prob_readers[p][st];
    }
    delete [] sum_prob_readers[p];
  }
  delete [] sum_prob_readers;
}

void TCFR::Write(unsigned int batch_base) {
  char dir[500], buf[500];
  sprintf(dir, "%s/%s.%u.%s.%i.%i.%i.%s.%s", Files::NewCFRBase(),
	  Game::GameName().c_str(), Game::NumPlayers(),
	  card_abstraction_.CardAbstractionName().c_str(), Game::NumRanks(),
	  Game::NumSuits(), Game::MaxStreet(),
	  betting_abstraction_.BettingAbstractionName().c_str(), 
	  cfr_config_.CFRConfigName().c_str());
  if (asymmetric_) {
    char buf2[20];
    sprintf(buf2, ".p%u", target_player_);
    strcat(dir, buf2);
  }
  Mkdir(dir);
  unsigned int num_players = Game::NumPlayers();
  Writer ***regret_writers = new Writer **[num_players];
  for (unsigned int p = 0; p < num_players; ++p) {
    regret_writers[p] = new Writer *[max_street_ + 1];
    for (unsigned int st = 0; st <= max_street_; ++st) {
      sprintf(buf, "%s/regrets.x.0.0.%u.%u.p%u.i", dir,
	      st, batch_base, p);
      regret_writers[p][st] = new Writer(buf);
    }
  }
  bool ***seen = new bool **[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    seen[st] = new bool *[num_players];
    for (unsigned int p = 0; p < num_players; ++p) {
      unsigned int num_nt = betting_tree_->NumNonterminals(p, st);
      seen[st][p] = new bool[num_nt];
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][p][i] = false;
      }
    }
  }
  WriteRegrets(data_, betting_tree_->Root(), regret_writers, seen);
  for (unsigned int p = 0; p < num_players; ++p) {
    for (unsigned int st = 0; st <= max_street_; ++st) {
      delete regret_writers[p][st];
    }
    delete [] regret_writers[p];
  }
  delete [] regret_writers;

  Writer ***sum_prob_writers = new Writer **[num_players];
  for (unsigned int p = 0; p < num_players; ++p) {
    // In asymmetric systems, only save sumprobs for target player
    if (asymmetric_ && p != target_player_) {
      sum_prob_writers[p] = NULL;
      continue;
    }
    sum_prob_writers[p] = new Writer *[max_street_ + 1];
    for (unsigned int st = 0; st <= max_street_; ++st) {
      if (! sumprob_streets_[st]) {
	sum_prob_writers[p][st] = NULL;
	continue;
      }
      sprintf(buf, "%s/sumprobs.x.0.0.%u.%u.p%u.i", dir,
	      st, batch_base, p);
      sum_prob_writers[p][st] = new Writer(buf);
    }
  }
  for (unsigned int st = 0; st <= max_street_; ++st) {
    for (unsigned int p = 0; p < num_players; ++p) {
      unsigned int num_nt = betting_tree_->NumNonterminals(p, st);
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][p][i] = false;
      }
    }
  }
  WriteSumprobs(data_, betting_tree_->Root(), sum_prob_writers, seen);
  for (unsigned int st = 0; st <= max_street_; ++st) {
    for (unsigned int p = 0; p < num_players; ++p) {
      delete [] seen[st][p];
    }
    delete [] seen[st];
  }
  delete [] seen;
  for (unsigned int p = 0; p < num_players; ++p) {
    if (asymmetric_ && p != target_player_) continue;
    for (unsigned int st = 0; st <= max_street_; ++st) {
      if (! sumprob_streets_[st]) continue;
      delete sum_prob_writers[p][st];
    }
    delete [] sum_prob_writers[p];
  }
  delete [] sum_prob_writers;
}

void TCFR::Run(void) {
  // Temporary?
  unsigned int num_preflop_buckets = buckets_.NumBuckets(0);
  g_preflop_vals = new double[num_preflop_buckets];
  g_preflop_nums = new unsigned long long int[num_preflop_buckets];
  for (unsigned int b = 0; b < num_preflop_buckets; ++b) {
    g_preflop_vals[b] = 0;
    g_preflop_nums[b] = 0ULL;
  }

  for (unsigned int i = 1; i < num_cfr_threads_; ++i) {
    cfr_threads_[i]->RunThread();
  }
  // Execute thread 0 in main execution thread
  fprintf(stderr, "Starting thread 0 in main thread\n");
  cfr_threads_[0]->Run();
  fprintf(stderr, "Finished main thread\n");
  for (unsigned int i = 1; i < num_cfr_threads_; ++i) {
    cfr_threads_[i]->Join();
    fprintf(stderr, "Joined thread %i\n", i);
  }

  // Temporary?
  unsigned int max_card = Game::MaxCard();
  Card hole_cards[2];
  for (Card hi = 1; hi <= max_card; ++hi) {
    hole_cards[0] = hi;
    for (Card lo = 0; lo < hi; ++lo) {
      hole_cards[1] = lo;
      unsigned int hcp = HCPIndex(0, hole_cards);
      unsigned int b = buckets_.Bucket(0, hcp);
      double val = g_preflop_vals[b];
      unsigned long long int num = g_preflop_nums[b];
      if (num > 0) {
	printf("%f ", val / (double)num);
	OutputTwoCards(hi, lo);
	printf(" (%u)\n", b);
	g_preflop_nums[b] = 0;
      }
    }
  }
  fflush(stdout);
  delete [] g_preflop_vals;
  delete [] g_preflop_nums;
}

void TCFR::RunBatch(unsigned int batch_size) {
  SeedRand(batch_base_);
  fprintf(stderr, "Seeding to %i\n", batch_base_);
  for (unsigned int i = 0; i < kNumPregenRNGs; ++i) {
    rngs_[i] = RandZeroToOne();
    // A hack.  We can end up generating 1.0 as an RNG because we are casting
    // doubles to floats.  Code elsewhere assumes RNGs are strictly less than
    // 1.0.
    if (rngs_[i] >= 1.0) rngs_[i] = 0.99999;
  }

  cfr_threads_ = new TCFRThread *[num_cfr_threads_];
  for (unsigned int i = 0; i < num_cfr_threads_; ++i) {
    TCFRThread *cfr_thread =
      new TCFRThread(betting_abstraction_, cfr_config_, buckets_,
		     batch_base_ + i, num_cfr_threads_, data_, target_player_,
		     rngs_, uncompress_, short_uncompress_,
		     pruning_thresholds_, sumprob_streets_, hvb_table_,
		     cards_to_indices_, batch_size);
    cfr_threads_[i] = cfr_thread;
  }

  fprintf(stderr, "Running batch base %i\n", batch_base_);
  Run();
  fprintf(stderr, "Finished running batch base %i\n", batch_base_);

  for (unsigned int i = 0; i < num_cfr_threads_; ++i) {
    total_process_count_ += cfr_threads_[i]->ProcessCount();
  }
  for (unsigned int i = 0; i < num_cfr_threads_; ++i) {
    total_full_process_count_ += cfr_threads_[i]->FullProcessCount();
  }

  for (unsigned int i = 0; i < num_cfr_threads_; ++i) {
    delete cfr_threads_[i];
  }
  delete [] cfr_threads_;
  cfr_threads_ = NULL;
}

void TCFR::Run(unsigned int start_batch_base, unsigned int batch_size,
	       unsigned int save_interval) {
  if (save_interval < num_cfr_threads_) {
    fprintf(stderr, "Save interval must be at least the number of threads\n");
    exit(-1);
  }

  if (save_interval % num_cfr_threads_ != 0) {
    fprintf(stderr, "Save interval should be multiple of number of threads\n");
    exit(-1);
  }

  if (start_batch_base > 0) {
    // Doesn't allow us to change number of threads
    unsigned int old_batch_base = start_batch_base - num_cfr_threads_;
    Read(old_batch_base);
  }

  total_process_count_ = 0ULL;
  total_full_process_count_ = 0ULL;

  batch_base_ = start_batch_base;
  while (true) {
    RunBatch(batch_size);

    // This is a little ugly.  If our save interval is 800, we want to save
    // at batches 800, 1600, etc., but not at batch 0 even though 0 % 800 == 0.
    // But if our save interval is 8 (and we have eight threads), then we
    // do want to save at batch 0.
    if (batch_base_ % save_interval == 0 &&
	(batch_base_ > 0 || save_interval == num_cfr_threads_)) {
      fprintf(stderr, "Process count: %llu\n", total_process_count_);
      fprintf(stderr, "Full process count: %llu\n", total_full_process_count_);
      time_t start_t = time(NULL);
      Write(batch_base_);
      fprintf(stderr, "Checkpointed batch base %u\n", batch_base_);
      time_t end_t = time(NULL);
      double diff_sec = difftime(end_t, start_t);
      fprintf(stderr, "Writing took %.1f seconds\n", diff_sec);
      total_process_count_ = 0ULL;
      total_full_process_count_ = 0ULL;
    }

    batch_base_ += num_cfr_threads_;
  }
}

void TCFR::Run(unsigned int start_batch_base, unsigned int end_batch_base,
	       unsigned int batch_size, unsigned int save_interval) {
  if ((end_batch_base + num_cfr_threads_ - start_batch_base) %
      num_cfr_threads_ != 0) {
    fprintf(stderr, "Batches to execute should be multiple of number of "
	    "threads\n");
    exit(-1);
  }
  if (save_interval % num_cfr_threads_ != 0) {
    fprintf(stderr, "Save interval should be multiple of number of threads\n");
    exit(-1);
  }
  if ((end_batch_base + num_cfr_threads_ - start_batch_base) %
      save_interval != 0) {
    fprintf(stderr, "Batches to execute should be multiple of save interval\n");
    exit(-1);
  }
  if (start_batch_base > 0) {
    // Doesn't allow us to change number of threads
    unsigned int old_batch_base = start_batch_base - num_cfr_threads_;
    Read(old_batch_base);
  }

  total_process_count_ = 0ULL;
  total_full_process_count_ = 0ULL;

  for (batch_base_ = start_batch_base;
       batch_base_ <= end_batch_base; batch_base_ += num_cfr_threads_) {
    RunBatch(batch_size);
    // This is a little ugly.  If our save interval is 800, we want to save
    // at batches 800, 1600, etc., but not at batch 0 even though 0 % 800 == 0.
    // But if our save interval is 8 (and we have eight threads), then we
    // do want to save at batch 0.
    if (batch_base_ % save_interval == 0 &&
	(batch_base_ > 0 || save_interval == num_cfr_threads_)) {
      fprintf(stderr, "Process count: %llu\n", total_process_count_);
      fprintf(stderr, "Full process count: %llu\n", total_full_process_count_);
      Write(batch_base_);
      fprintf(stderr, "Checkpointed batch base %u\n", batch_base_);
      total_process_count_ = 0ULL;
      total_full_process_count_ = 0ULL;
    }
  }
}

// Returns a pointer to the allocation buffer after this node and all of its
// descendants.
// New scheme that supports multiplayer:
// Terminal
//   Byte 0:     Number of remaining players
//   Byte 1-7:   Booleans indicating whether each player remaining
//   Bytes 8...? Contributions
// Nonterminal
//   Byte 0:      0
//   Byte 1:      Street
//   Byte 2:      Num succs
//   Byte 3:      Fold succ index
//   Bytes 4-7:   Player acting
//   Bytes 8...?  Contributions
//   Byte ???:    Beginning of succ ptrs
// Now:
// First byte is node type (0=showdown, 1=P1 fold, 2=P2 fold,
// 3=P1-choice nonterminal,4=P2-choice nonterminal)
// If nonterminal, second byte is street (bottom two bits) and
// granularity (remaining bits).
// If nonterminal, third byte is num succs
// If nonterminal, fourth byte is fold succ index
// If terminal, bytes 4-7 are the pot-size/2
// Remainder is only for nonterminals:
// The next num-succs * 8 bytes are for the succ ptrs
// For each bucket
//   num-succs * sizeof(T_REGRET) for the regrets
//   num-succs * sizeof(T_SUM_PROB) for the sum-probs
// A little padding is added at the end if necessary to make the number of
// bytes for a node be a multiple of 8.
unsigned char *TCFR::Prepare(unsigned char *ptr, Node *node,
			     bool *folded, unsigned int *contributions,
			     unsigned int last_bet_to,
			     unsigned long long int ***offsets) {
  unsigned int num_players = Game::NumPlayers();
  if (node->Terminal()) {
    if (num_players > 7) {
      fprintf(stderr, "Only support up to 7 players\n");
      exit(-1);
    }
    unsigned int num_remaining = 0;
    for (unsigned int p = 0; p < num_players; ++p) {
      if (folded[p]) {
	ptr[p + 1] = 0;
      } else {
	ptr[p + 1] = 1;
	++num_remaining;
      }
    }
    ptr[0] = num_remaining;
    for (unsigned int p = 0; p < num_players; ++p) {
      *(unsigned int *)(ptr + 8 + p * 4) = contributions[p];
    }
    return ptr + 8 + num_players * 4;
  }
  unsigned int st = node->Street();
  ptr[0] = 0;
  ptr[1] = st;
  unsigned int num_succs = node->NumSuccs();
  ptr[2] = num_succs;
  unsigned int fsi = 255;
  if (node->HasFoldSucc()) fsi = node->FoldSuccIndex();
  ptr[3] = fsi;
  unsigned int pa = node->PlayerActing();
  *((unsigned int *)(ptr + 4)) = pa;
  for (unsigned int p = 0; p < num_players; ++p) {
    *(unsigned int *)(ptr + 8 + p * 4) = contributions[p];
  }
  unsigned char *succ_ptr = ptr + 8 + num_players * 4;

  unsigned char *ptr1 = succ_ptr + num_succs * 8;
  if (num_succs > 1) {
    unsigned int num_buckets = buckets_.NumBuckets(st);
    for (unsigned int b = 0; b < num_buckets; ++b) {
      // Regrets
      if (quantized_streets_[st]) {
	for (unsigned int s = 0; s < num_succs; ++s) {
	  *ptr1 = 0;
	  ++ptr1;
	}
      } else if (short_quantized_streets_[st]) {
	for (unsigned int s = 0; s < num_succs; ++s) {
	  *(unsigned short *)ptr1 = 0;
	  ptr1 += 2;
	}
      } else {
	for (unsigned int s = 0; s < num_succs; ++s) {
	  *((T_REGRET *)ptr1) = 0;
	  ptr1 += sizeof(T_REGRET);
	}
      }
      // Sumprobs
      if (sumprob_streets_[st]) {
	if (! asymmetric_ || pa == target_player_) {
	  for (unsigned int s = 0; s < num_succs; ++s) {
	    *((T_SUM_PROB *)ptr1) = 0;
	    ptr1 += sizeof(T_SUM_PROB);
	  }
	}
      }
    }
  }

  for (unsigned int s = 0; s < num_succs; ++s) {
    unsigned long long int ull_offset = ptr1 - data_;
    Node *succ = node->IthSucc(s);
    if (! succ->Terminal()) {
      unsigned int succ_st = succ->Street();
      unsigned int succ_pa = succ->PlayerActing();
      unsigned int succ_nt = succ->NonterminalID();
      unsigned int succ_offset = offsets[succ_st][succ_pa][succ_nt];
      if (succ_offset != kMaxUInt) {
	*((unsigned long long int *)(succ_ptr + s * 8)) = succ_offset;
	continue;
      } else {
	offsets[succ_st][succ_pa][succ_nt] = ull_offset;
      }
    }
    *((unsigned long long int *)(succ_ptr + s * 8)) = ull_offset;
    if (s == fsi) {
      unique_ptr<bool []> new_folded(new bool[num_players]);
      for (unsigned int p = 0; p < num_players; ++p) {
	new_folded[p] = folded[p] || p == pa;
      }
      ptr1 = Prepare(ptr1, succ, new_folded.get(), contributions, last_bet_to,
		     offsets);
    } else if (node->HasCallSucc() && s == node->CallSuccIndex()) {
      // If I call a bet, need to set new_contributions.
      unique_ptr<unsigned int []>
	new_contributions(new unsigned int[num_players]);
      for (unsigned int p = 0; p < num_players; ++p) {
	if (p == pa) {
	  new_contributions[p] = last_bet_to;
	} else {
	  new_contributions[p] = contributions[p];
	}
      }
      ptr1 = Prepare(ptr1, succ, folded, new_contributions.get(), last_bet_to,
		     offsets);
    } else {
      unsigned int new_bet_to;
      if (num_players == 2) {
	Node *call = succ->IthSucc(succ->CallSuccIndex());
	unsigned int new_pot_size = call->PotSize();
	new_bet_to = new_pot_size / 2;
      } else {
	new_bet_to = succ->LastBetTo();
      }
      unique_ptr<unsigned int []>
	new_contributions(new unsigned int[num_players]);
      for (unsigned int p = 0; p < num_players; ++p) {
	if (p == pa) {
	  new_contributions[p] = new_bet_to;
	} else {
	  new_contributions[p] = contributions[p];
	}
      }
      ptr1 = Prepare(ptr1, succ, folded, new_contributions.get(), new_bet_to,
		     offsets);
    }
  }
  return ptr1;
}

void TCFR::MeasureTree(Node *node, bool ***seen,
		       unsigned long long int *allocation_size) {
  if (node->Terminal()) {
    *allocation_size += 8 + Game::NumPlayers() * 4;
    return;
  }

  unsigned int st = node->Street();
  unsigned int pa = node->PlayerActing();
  unsigned int nt = node->NonterminalID();
  if (seen[st][pa][nt]) {
    return;
  }
  seen[st][pa][nt] = true;
  
  // This is the number of bytes needed for everything else (e.g.,
  // num-succs).
  unsigned int this_sz = 8 + Game::NumPlayers() * 4;

  unsigned int num_succs = node->NumSuccs();
  // Eight bytes per succ
  this_sz += num_succs * 8;
  if (num_succs > 1) {
    // A regret and a sum-prob for each bucket and succ
    unsigned int nb = buckets_.NumBuckets(st);
    if (quantized_streets_[st]) {
      this_sz += nb * num_succs;
    } else if (short_quantized_streets_[st]) {
      this_sz += nb * num_succs * 2;
    } else {
      this_sz += nb * num_succs * sizeof(T_REGRET);
    }
    if (sumprob_streets_[st]) {
      if (! asymmetric_ || node->PlayerActing() == target_player_) {
	this_sz += nb * num_succs * sizeof(T_SUM_PROB);
      }
    }
  }

  *allocation_size += this_sz;

  for (unsigned int s = 0; s < num_succs; ++s) {
    MeasureTree(node->IthSucc(s), seen, allocation_size);
  }
}

static unsigned int PrecedingPlayer(unsigned int p) {
  if (p == 0) return Game::NumPlayers() - 1;
  else        return p - 1;
}

// Allocate one contiguous block of memory that has successors, street,
// num-succs, regrets, sum-probs, showdown/fold flag, pot-size/2.
void TCFR::Prepare(void) {
  unsigned int max_street = Game::MaxStreet();
  bool ***seen = new bool **[max_street + 1];
  for (unsigned int st = 0; st <= max_street; ++st) {
    seen[st] = new bool *[num_players_];
    for (unsigned int pa = 0; pa < num_players_; ++pa) {
      unsigned int num_nt = betting_tree_->NumNonterminals(pa, st);
      seen[st][pa] = new bool[num_nt];
      for (unsigned int i = 0; i < num_nt; ++i) {
	seen[st][pa][i] = false;
      }
    }
  }
  // Use an unsigned long long int, but succs are four-byte
  unsigned long long int allocation_size = 0;
  MeasureTree(betting_tree_->Root(), seen, &allocation_size);

  for (unsigned int st = 0; st <= max_street; ++st) {
    for (unsigned int pa = 0; pa < num_players_; ++pa) {
      delete [] seen[st][pa];
    }
    delete [] seen[st];
  }
  delete [] seen;

  // Should get amount of RAM from method in Files class
  if (allocation_size > 2050000000000ULL) {
    fprintf(stderr, "Allocation size %llu too big\n", allocation_size);
    exit(-1);
  }
  fprintf(stderr, "Allocation size: %llu\n", allocation_size);
  data_ = new unsigned char[allocation_size];
  if (data_ == NULL) {
    fprintf(stderr, "Could not allocate\n");
    exit(-1);
  }

  unsigned long long int ***offsets =
    new unsigned long long int **[max_street + 1];
  for (unsigned int st = 0; st <= max_street; ++st) {
    offsets[st] = new unsigned long long int *[num_players_];
    for (unsigned int pa = 0; pa < num_players_; ++pa) {
      unsigned int num_nt = betting_tree_->NumNonterminals(pa, st);
      offsets[st][pa] = new unsigned long long int[num_nt];
      for (unsigned int i = 0; i < num_nt; ++i) {
	offsets[st][pa][i] = kMaxUInt;
      }
    }
  }
  unique_ptr<bool []> folded(new bool[num_players_]);
  for (unsigned int p = 0; p < num_players_; ++p) folded[p] = false;
  unique_ptr<unsigned int []> contributions(new unsigned int[num_players_]);
  // Assume the big blind is last to act preflop
  // Assume the small blind is prior to the big blind
  unsigned int big_blind_p = PrecedingPlayer(Game::FirstToAct(0));
  unsigned int small_blind_p = PrecedingPlayer(big_blind_p);
  for (unsigned int p = 0; p < num_players_; ++p) {
    if (p == small_blind_p) {
      contributions[p] = Game::SmallBlind();
    } else if (p == big_blind_p) {
      contributions[p] = Game::BigBlind();
    } else {
      contributions[p] = 0;
    }
  }
  unsigned char *end = Prepare(data_, betting_tree_->Root(), folded.get(),
			       contributions.get(), Game::BigBlind(), offsets);
  unsigned long long int sz = end - data_;
  if (sz != allocation_size) {
    fprintf(stderr, "Didn't fill expected number of bytes: sz %llu as %llu\n",
	    sz, allocation_size);
    exit(-1);
  }

  for (unsigned int st = 0; st <= max_street; ++st) {
    for (unsigned int pa = 0; pa < num_players_; ++pa) {
      delete [] offsets[st][pa];
    }
    delete [] offsets[st];
  }
  delete [] offsets;
}

TCFR::TCFR(const CardAbstraction &ca, const BettingAbstraction &ba,
	   const CFRConfig &cc, const Buckets &buckets,
	   unsigned int num_threads, unsigned int target_player) :
  card_abstraction_(ca), betting_abstraction_(ba), cfr_config_(cc),
  buckets_(buckets) {
  fprintf(stderr, "Full evaluation if regrets close; threshold %u\n",
	  cfr_config_.CloseThreshold());
  time_t start_t = time(NULL);
  asymmetric_ = betting_abstraction_.Asymmetric();
  num_players_ = Game::NumPlayers();
  target_player_ = target_player;
  num_cfr_threads_ = num_threads;
  fprintf(stderr, "Num threads: %i\n", num_cfr_threads_);
  max_street_ = Game::MaxStreet();
  for (unsigned int st = 0; st <= max_street_; ++st) {
    if (buckets_.None(st)) {
      fprintf(stderr, "TCFR expects buckets on all streets\n");
      exit(-1);
    }
  }

  BoardTree::Create();
  BoardTree::BuildBoardCounts();
  BoardTree::BuildPredBoards();
  
  pruning_thresholds_ = new unsigned int[max_street_];
  const vector<unsigned int> &v = cfr_config_.PruningThresholds();
  for (unsigned int st = 0; st <= max_street_; ++st) {
    pruning_thresholds_[st] = v[st];
  }

  sumprob_streets_ = new bool[max_street_ + 1];
  const vector<unsigned int> &ssv = cfr_config_.SumprobStreets();
  unsigned int num_ssv = ssv.size();
  if (num_ssv == 0) {
    for (unsigned int st = 0; st <= max_street_; ++st) {
      sumprob_streets_[st] = true;
    }
  } else {
    for (unsigned int st = 0; st <= max_street_; ++st) {
      sumprob_streets_[st] = false;
    }
    for (unsigned int i = 0; i < num_ssv; ++i) {
      unsigned int st = ssv[i];
      sumprob_streets_[st] = true;
    }
  }

  quantized_streets_ = new bool[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    quantized_streets_[st] = false;
  }
  const vector<unsigned int> &qsv = cfr_config_.QuantizedStreets();
  unsigned int num_qsv = qsv.size();
  for (unsigned int i = 0; i < num_qsv; ++i) {
    unsigned int st = qsv[i];
    quantized_streets_[st] = true;
  }
  short_quantized_streets_ = new bool[max_street_ + 1];
  for (unsigned int st = 0; st <= max_street_; ++st) {
    short_quantized_streets_[st] = false;
  }
  const vector<unsigned int> &sqsv = cfr_config_.ShortQuantizedStreets();
  unsigned int num_sqsv = sqsv.size();
  for (unsigned int i = 0; i < num_sqsv; ++i) {
    unsigned int st = sqsv[i];
    short_quantized_streets_[st] = true;
  }

  if (betting_abstraction_.Asymmetric()) {
    betting_tree_.reset(BettingTree::BuildAsymmetricTree(betting_abstraction_,
							 target_player_));
  } else {
    betting_tree_.reset(BettingTree::BuildTree(betting_abstraction_));
  }

  Prepare();

  rngs_ = new float[kNumPregenRNGs];
  uncompress_ = new unsigned int[256];
  for (unsigned int c = 0; c <= 255; ++c) {
    uncompress_[c] = UncompressRegret(c);
  }
  short_uncompress_ = new unsigned int[65536];
  for (unsigned int c = 0; c <= 65535; ++c) {
    short_uncompress_[c] = UncompressRegretShort(c);
  }

  if (cfr_config_.HVBTable()) {
    // How much extra space does this require?  2.5b hands times 4 bytes
    // for hand value is 10 gigs - minus size of HandTree.
    char buf[500];
    string max_street_bucketing = card_abstraction_.Bucketing(max_street_);
    sprintf(buf, "%s/hvb.%s.%u.%u.%u.%s", Files::StaticBase(),
	    Game::GameName().c_str(), Game::NumRanks(), Game::NumSuits(),
	    max_street_, max_street_bucketing.c_str());
    unsigned int num_boards = BoardTree::NumBoards(max_street_);
    unsigned int num_hole_card_pairs = Game::NumHoleCardPairs(max_street_);
    unsigned int num_hands = num_boards * num_hole_card_pairs;
    long long int bytes = 4;
    if (buckets_.NumBuckets(max_street_) <= 65536) bytes += 2;
    else                                           bytes += 4;
    long long int total_bytes = ((long long int)num_hands) * bytes;
    Reader reader(buf);
    if (reader.FileSize() != total_bytes) {
      fprintf(stderr, "File size %lli expected %lli\n", reader.FileSize(),
	      total_bytes);
    }
    hvb_table_ = new unsigned char[total_bytes];
    for (long long int i = 0; i < total_bytes; ++i) {
      if ((i % 1000000000LL) == 0) {
	fprintf(stderr, "i %lli/%lli\n", i, total_bytes);
      }
      hvb_table_[i] = reader.ReadUnsignedCharOrDie();
    }
  } else {
    HandValueTree::Create();
    hvb_table_ = NULL;
  }

  if (cfr_config_.HVBTable()) {
    cards_to_indices_ = new unsigned char **[max_street_ + 1];
    unsigned int max_card = Game::MaxCard();
    for (unsigned int st = 0; st <= max_street_; ++st) {
      unsigned int num_boards = BoardTree::NumBoards(st);
      unsigned int num_board_cards = Game::NumBoardCards(st);
      cards_to_indices_[st] = new unsigned char *[num_boards];
      for (unsigned int bd = 0; bd < num_boards; ++bd) {
	const Card *board = BoardTree::Board(st, bd);
	cards_to_indices_[st][bd] = new unsigned char[max_card + 1];
	unsigned int num_lower = 0;
	for (unsigned int c = 0; c <= max_card; ++c) {
	  // It's OK if we assign a value to a card that is on the board.
	  cards_to_indices_[st][bd][c] = c - num_lower;
	  for (unsigned int i = 0; i < num_board_cards; ++i) {
	    if (c == board[i]) {
	      ++num_lower;
	      break;
	    }
	  }
	}
      }
    }
  } else {
    cards_to_indices_ = NULL;
  }

  time_t end_t = time(NULL);
  double diff_sec = difftime(end_t, start_t);
  fprintf(stderr, "Initialization took %.1f seconds\n", diff_sec);
}

TCFR::~TCFR(void) {
  if (cards_to_indices_) {
    for (unsigned int st = 0; st <= max_street_; ++st) {
      unsigned int num_boards = BoardTree::NumBoards(st);
      for (unsigned int bd = 0; bd < num_boards; ++bd) {
	delete [] cards_to_indices_[st][bd];
      }
      delete [] cards_to_indices_[st];
    }
    delete [] cards_to_indices_;
  }
  delete [] hvb_table_;
  delete [] pruning_thresholds_;
  delete [] uncompress_;
  delete [] short_uncompress_;
  delete [] rngs_;
  delete [] data_;
  delete [] quantized_streets_;
  delete [] short_quantized_streets_;
  delete [] sumprob_streets_;
}
