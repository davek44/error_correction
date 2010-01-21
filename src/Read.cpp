#include "Read.h"
//#include "prefix_tree.h"
#include "bithash.h"
//#include "dawg.h"
#include <iostream>
#include <math.h>
#include <algorithm>
#include <set>
#include <queue>

#define TESTING false
#define DEBUG false

////////////////////////////////////////////////////////////
// corrections_compare
//
// Simple class to compare to corrected_read's in the
// priority queue
////////////////////////////////////////////////////////////
class corrections_compare {
public:
  //  corrections_compare() {};
  bool operator() (const corrected_read* lhs, const corrected_read* rhs) const {
    //return lhs->likelihood < rhs->likelihood;
    if(lhs->likelihood < rhs->likelihood)
      return true;
    else if(lhs->likelihood > rhs->likelihood)
      return false;
    else
      return lhs->region_edits > rhs->region_edits;
  }
};

////////////////////////////////////////////////////////////
// Read (constructor)
//
// Make shallow copies of sequence and untrusted, and
// convert quality value string to array of probabilities
////////////////////////////////////////////////////////////
Read::Read(const string & h, const unsigned int* s, const string & q, vector<int> & u, const int rl)
  :untrusted(u) {

  header = h;
  read_length = rl;
  seq = new unsigned int[read_length];
  prob = new float[read_length];
  for(int i = 0; i < read_length; i++) {
    seq[i] = s[i];    
    // quality values of 0,1 lead to p < .25
    prob[i] = max(.25, 1.0-pow(10.0,-(q[i]-33)/10.0));
  }
  trusted_read = 0;
}

Read::~Read() {
  delete[] seq;
  delete[] prob;
  if(trusted_read != 0)
    delete trusted_read;
}

////////////////////////////////////////////////////////////
// correct
//
// Find the set of corrections with maximum likelihood
// that result in all trusted kmers
////////////////////////////////////////////////////////////
//bool Read::correct(prefix_tree *trusted, ofstream & out) {
bool Read::correct(bithash *trusted, ofstream & out) {
//bool Read::correct(dawg *trusted, ofstream & out) {
  // determine region to consider
  vector<short> region = error_region();

  // filter really bad reads  
  float avgprob = 0;
  for(int i = 0; i < region.size(); i++)
    avgprob += prob[region[i]];
  avgprob /= region.size();
  
  int badnt = 0;
  for(int i = 0; i < region.size(); i++)
    if(prob[region[i]] < .95)
      badnt += 1;

  bool forfeit_easily = false;
  if(!TESTING) {
    if(badnt >= 8)
      forfeit_easily = true;;

    // filter reads that look like low coverage
    if(untrusted.size() > .95*(read_length-k+1) && avgprob > .99) {
      out << header << "\t" << print_seq() << "\t+" << endl;
      return false;
    }
  }

  // sort by quality
  quality_quicksort(region, 0, region.size()-1);

  // data structure for corrected_reads sorted by likelihood
  priority_queue< corrected_read*, vector<corrected_read*>, corrections_compare > cpq;
  int max_pq = 0;

  // add initial reads
  corrected_read *cr, *next_cr;
  short edit_i = region[0];
  float like;
  for(short nt = 0; nt < 4; nt++) {
    if(seq[edit_i] == nt) {
      next_cr = new corrected_read(untrusted, 1.0, 1, true);
    } else {
      like = (1.0-prob[edit_i])/3.0 / prob[edit_i];
      next_cr = new corrected_read(untrusted, like, 1, false);
      next_cr->corrections.push_back(new correction(edit_i, nt));
    }
    
    // add to priority queue
    cpq.push(next_cr);
  }

  // initialize likelihood parameters
  bool quit_early = false;
  trusted_read = 0;
  float trusted_likelihood;
  int untrusted_count;

  ////////////////////////////////////////
  // process corrected reads
  ////////////////////////////////////////
  while(cpq.size() > 0) {
    if(cpq.size() > max_pq)
      max_pq = cpq.size();
    
    if(!TESTING) {
      // give up on read if it was marked and no trusted reads have been found by 30k
      if(forfeit_easily && trusted_read == 0 && cpq.size() > 30000) {
	quit_early = true;
	break;
      }
    }
    
    // give up on read if pq is too big
    if(cpq.size() > 400000) {
      cout << "queue is too large for " << header << endl;
      if(trusted_read != 0) {
	delete trusted_read;
	trusted_read = 0;
      }
      break;
    }

    cr = cpq.top();
    cpq.pop();

    // print read  
    if(DEBUG) {
      if(header == "@8812") {
	printf("\nQueue size: %d\n",cpq.size());
	printf("Region edit: %d\nCorrections:",cr->region_edits);
	for(int i = 0; i < cr->corrections.size(); i++) {
	  printf("%d: %d, ", cr->corrections[i]->index, cr->corrections[i]->to);
	}
	printf("\n");
	cout << print_corrected(cr) << endl;
	printf("Like: %f\n",cr->likelihood);
      } 
    }

    // save for later comparison
    untrusted_count = (signed int)cr->untrusted.size();

    // check trust only if unchecked
    if(!cr->checked) {

      if(trusted_read != 0) {
	// if a corrected read exists, compare likelihoods and if likelihood is too low, break loop return true
	if(cr->likelihood < trusted_likelihood*trust_spread_t) {
	  delete cr;
	  break;
	}
      } else {
	// if no corrected read exists and likelihood is too low, break loop return false
	if(cr->likelihood < correct_min_t) {
	  delete cr;
	  break;
	}
      }

      // check for all kmers trusted
      if(check_trust(cr, trusted)) {
	if(trusted_read == 0) {
	  // if yes, and first trusted read, save
	  trusted_read = cr;
	  trusted_likelihood = cr->likelihood;
	} else {
	  // output ambiguous corrections for testing
	  if(TESTING)
	    out << header << "\t" << print_seq() << "\t" << print_corrected(trusted_read) << "\t" << print_corrected(cr) << endl;

	  // if yes, and if trusted read exists delete trusted_read, break loop
	  delete trusted_read;
	  delete cr;
	  trusted_read = 0;
	  break;
	}
      }
    }
    
    // if untrusted sharply increases, just bail
    if(((signed int)cr->untrusted.size() - untrusted_count) < k/3) {    

      // determine next nt flips
      short region_edit = cr->region_edits;
      if (region_edit < region.size()) {
	edit_i = region[region_edit];
	
	// add relatives
	for(short nt = 0; nt < 4; nt++) {
	  if(seq[edit_i] == nt) {
	    // if no actual edit, copy over but move on to next edit
	    next_cr = new corrected_read(cr->corrections, cr->untrusted, cr->likelihood, region_edit+1, true);
	  } else {
	    // if actual edit, calculate new likelihood
	    like = cr->likelihood * (1.0-prob[edit_i])/3.0 / prob[edit_i];
	    
	    // if thresholds ok, add new correction
	    if(trusted_read != 0) {
	      if(like < trusted_likelihood*trust_spread_t)
		continue;
	    } else {
	      // must consider spread or risk missing a case of ambiguity
	      if(like < correct_min_t*trust_spread_t)
		continue;
	    }
	    
	    next_cr = new corrected_read(cr->corrections, cr->untrusted, like, region_edit+1, false);
	    next_cr->corrections.push_back(new correction(edit_i, nt));
	  }
	  
	  // add to priority queue
	  cpq.push(next_cr);
	}
      }
    }	  

    // if not the saved max trusted, delete
    if(trusted_read != cr) {
      delete cr;
    }
  }

  // delete memory from rpq
  // I bet it calls every corrected_read's destructor
  // but I could do it myself
  while(cpq.size() > 0) {
    cr = cpq.top();
    cpq.pop();
    delete cr;
  }
  
  //int t = 0;
  //if(trusted_read != 0)
  //  t = 1;  
  //cout << header << "\t" << max_pq << "\t" << avgprob << "\t" << badnt << "\t" << region.size() << "\t" << t << endl;

  if(trusted_read != 0) {
    out << header << "\t" << print_seq() << "\t" << print_corrected(trusted_read) << endl;
    return true;
  } else {    
    if(!TESTING) {
      if(quit_early)
	out << header << "\t" << print_seq() << "\t." << endl;
      else
	out << header << "\t" << print_seq() << "\t-" << endl;
    }
    return false;
  }
}

////////////////////////////////////////////////////////////
// print_seq
////////////////////////////////////////////////////////////
string Read::print_seq() {
  char nts[5] = {'A','C','G','T','N'};
  string sseq;
  for(int i = 0; i < read_length; i++)
    sseq.push_back(nts[seq[i]]);
  return sseq;
}

////////////////////////////////////////////////////////////
// print_corrected
////////////////////////////////////////////////////////////
string Read::print_corrected(corrected_read* cr) {
  char nts[5] = {'A','C','G','T','N'};
  string sseq;
  int correct_i;
  for(int i = 0; i < read_length; i++) {
    correct_i = -1;
    for(int c = 0; c < cr->corrections.size(); c++) {
      if(cr->corrections[c]->index == i)
	correct_i = c;
    }
    if(correct_i != -1)
      sseq.push_back(nts[cr->corrections[correct_i]->to]);
    else
      sseq.push_back(nts[seq[i]]);
  }
  return sseq;
}
  

////////////////////////////////////////////////////////////
// error_region
//
// Find region of the read to consider for errors based
// on the pattern of untrusted kmers
////////////////////////////////////////////////////////////
vector<short> Read::error_region() {
  // find read indexes to consider
  vector<short> region;
  if(untrusted_intersect(region)) {
    // if front kmers can reach region, there may be more
    // errors in the front

    short f = region.front();
    short b = region.back();

    if(k-1 >= f) {
      // extend to front
      for(short i = f-1; i >= 0; i--)
	region.push_back(i);
    }
    if(read_length-k <= b) {
      // extend to back
      for(short i = b+1; i < read_length; i++)
	region.push_back(i);
    }
  } else
    untrusted_union(region);

  return region;
}
    
////////////////////////////////////////////////////////////
// untrusted_intersect
//
// Compute the intersection of the untrusted kmers as
// start,end return true if it's non-empty or false
// otherwise
////////////////////////////////////////////////////////////
bool Read::untrusted_intersect(vector<short> & region) {
  int start = 0;
  int end = read_length-1;

  int u;
  for(int i = 0; i < untrusted.size(); i++) {
    u = untrusted[i];

    // if overlap
    if(start <= u+k-1 && u <= end) {
      // take intersection
      start = max(start, u);
      end = min(end, u+k-1);
    } else {
      // intersection is empty
      return false;   
    }
  }
    
  // intersection is non-empty
  for(short i = start; i <= end; i++)
    region.push_back(i);
  return true;
}

////////////////////////////////////////////////////////////
// untrusted_union
//
// Compute the union of the untrusted kmers, though not
////////////////////////////////////////////////////////////
void Read::untrusted_union(vector<short> & region) {
  short u;
  set<short> region_set;
  for(int i = 0; i < untrusted.size(); i++) {
    u = untrusted[i];

    for(short ui = u; ui < u+k; ui++)
      region_set.insert(ui);
  }

  set<short>::iterator it;
  for(it = region_set.begin(); it != region_set.end(); it++)
    region.push_back(*it);      
}

////////////////////////////////////////////////////////////
// quality_quicksort
//
// Sort the indexes from lowest probability of an accurate
// basecall to highest
////////////////////////////////////////////////////////////
void Read::quality_quicksort(vector<short> & indexes, int left, int right) {
  int i = left, j = right;  
  short tmp;
  float pivot = prob[indexes[(left + right) / 2]];
 
  /* partition */
  while (i <= j) {
    while (prob[indexes[i]] < pivot)
      i++;    
    while (prob[indexes[j]] > pivot)      
      j--;
    if (i <= j) {
      tmp = indexes[i];
      indexes[i] = indexes[j];
      indexes[j] = tmp;
      i++;
      j--;
    }
  }

  /* recursion */
  if (left < j)
    quality_quicksort(indexes, left, j);
  if (i < right)
    quality_quicksort(indexes, i, right);  
}

////////////////////////////////////////////////////////////
// check_trust
//
// Given a corrected read and data structure holding
// trusted kmers, update the corrected_reads's vector
// of untrusted kmers and return true if it's now empty
////////////////////////////////////////////////////////////
//bool Read::check_trust(corrected_read *cr, prefix_tree *trusted) {
bool Read::check_trust(corrected_read *cr, bithash *trusted) {
//bool Read::check_trust(corrected_read *cr, dawg *trusted) {
  // original read HAS errors
  if(cr->corrections.empty())
    return false;

  // make corrections to sequence, saving nt's to fix later
  vector<int> seqsave;
  int i;
  for(i = 0; i < cr->corrections.size(); i++) {
    seqsave.push_back(seq[cr->corrections[i]->index]);
    seq[cr->corrections[i]->index] = cr->corrections[i]->to;
  }

  // clear untrusted kmer list to reinitialize
  vector<int> untrusted_save(cr->untrusted);
  cr->untrusted.clear();

  int edit = cr->corrections.back()->index;
  int kmer_start = max(0, edit-k+1);
  int kmer_end = min(edit, read_length-k);

  // add untrusted kmers before edit
  i = 0;
  //vector<int> old_untrusted;
  while(i < untrusted_save.size() && untrusted_save[i] < kmer_start) {
    cr->untrusted.push_back(untrusted_save[i]);
    //old_untrusted.push_back(untrusted_save[i]);
    i++;
  }

  // check affected kmers  
  /*
  for(i = kmer_start; i <= kmer_end; i++) {
    // check kmer
    if(!trusted->check(&seq[i]))
      cr->untrusted.push_back(i);
      //old_untrusted.push_back(i);
  }
  */ 

  long long unsigned kmermap;
  // check first kmer and save map value
  if(!trusted->check(&seq[kmer_start], kmermap))
    cr->untrusted.push_back(kmer_start);
  for(i = kmer_start+1; i <= kmer_end; i++) {
    // check kmer using map value
    if(!trusted->check(kmermap, seq[i-1], seq[i+k-1]))
      cr->untrusted.push_back(i);
  }

  // compare new way and old way
  /*
  if(cr->untrusted != old_untrusted) {
    cout << "different" << endl;
    for(int i = 0; i < old_untrusted.size(); i++)
      cout << old_untrusted[i] << " ";
    cout << endl;
    for(int i = 0; i < cr->untrusted.size(); i++)
      cout << cr->untrusted[i] << " ";
    cout << endl;
  }
  */

  // add untrusted kmers after edit
  for(i = 0; i < untrusted_save.size(); i++) {
    if(untrusted_save[i] > kmer_end)
      cr->untrusted.push_back(untrusted_save[i]);
  }

  // fix sequence
  for(i = 0; i < cr->corrections.size(); i++)
    seq[cr->corrections[i]->index] = seqsave[i];

  return(cr->untrusted.empty());
}
