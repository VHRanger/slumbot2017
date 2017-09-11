#ifndef _PARAMS_
#define _PARAMS_

#include <string>
#include <vector>

using namespace std;

enum ParamType {
  P_STRING,
  P_INT,
  P_DOUBLE,
  P_BOOLEAN
};

struct ParamValue {
  bool set;
  string s;
  int i;
  double d;
};

class Params {
public:
  Params(void);
  ~Params(void);
  void AddParam(const string &name, ParamType ptype);
  void ReadFromFile(const char *filename);
  bool IsSet(const char *name) const;
  string GetStringValue(const char *name) const;
  int GetIntValue(const char *name) const;
  double GetDoubleValue(const char *name) const;
  bool GetBooleanValue(const char *name) const;
private:
  unsigned int GetParamIndex(const char *name) const;

  vector<string> param_names_;
  vector<ParamType> param_types_;
  vector<ParamValue> param_values_;
};

#endif
