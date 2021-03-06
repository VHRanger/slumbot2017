#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "split.h"

using namespace std;

void Split(const char *line, char sep, bool allow_empty,
	   vector<string> *comps) {
  comps->clear();
  unsigned int len = strlen(line);
  unsigned int i = 0;
  // Used to have while (i < len), but this does the wrong thing when
  // allow_empty is true and the line passed in ends in a separator
  // character.
  while (true) {
    unsigned int j = i;
    while (i < len && line[i] != sep) ++i;
    string s(line, j, i - j);
    // Strip any trailing \n
    if (s.size() > 0) {
      if (s[s.size() - 1] == '\n') {
	s.resize(s.size() - 1);
      }
    }
    if (s != "" || allow_empty) {
      // s can be empty if there was only new line in given field
      comps->push_back(s);
    }
    if (i == len) break;
    ++i; // Skip past separating character
  }
}

void ParseDoubles(const string &s, vector<double> *values) {
  vector<string> comps;
  Split(s.c_str(), ',', false, &comps);
  unsigned int num_comps = comps.size();
  values->resize(num_comps);
  double v;
  for (unsigned int i = 0; i < num_comps; ++i) {
    const string &cs = comps[i];
    if (sscanf(cs.c_str(), "%lf", &v) != 1) {
      fprintf(stderr, "Couldn't parse double value: %s\n",
	      s.c_str());
      exit(-1);
    }
    (*values)[i] = v;
  }
}

void ParseInts(const string &s, vector<int> *values) {
  vector<string> comps;
  Split(s.c_str(), ',', false, &comps);
  unsigned int num_comps = comps.size();
  values->resize(num_comps);
  int val;
  for (unsigned int i = 0; i < num_comps; ++i) {
    const string &cs = comps[i];
    if (sscanf(cs.c_str(), "%i", &val) != 1) {
      fprintf(stderr, "Couldn't parse int value: %s\n", s.c_str());
      exit(-1);
    }
    (*values)[i] = val;
  }
}

void ParseUnsignedInts(const string &s, vector<unsigned int> *values) {
  vector<string> comps;
  Split(s.c_str(), ',', false, &comps);
  unsigned int num_comps = comps.size();
  values->resize(num_comps);
  unsigned int u;
  for (unsigned int i = 0; i < num_comps; ++i) {
    const string &cs = comps[i];
    if (sscanf(cs.c_str(), "%u", &u) != 1) {
      fprintf(stderr, "Couldn't parse unsigned int value: %s\n",
	      s.c_str());
      exit(-1);
    }
    (*values)[i] = u;
  }
}

