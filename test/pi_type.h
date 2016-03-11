#include "dd2tuple.h"
#include <string>

struct PI_Shapes
{
  pi_string color;
  int x, y, shapesize;
  pi_vector<int> nums;
  pi_vector<pi_string> vs;
  pi_vector<pi_vector<pi_string>> vvs;

  PI_Shapes(Allocator *alloc)
    : color(alloc),
      nums(alloc),
      vs(alloc),
      vvs(alloc)
  {}
};

void write_pi_type(int domainid);

RTI_ADAPT_STRUCT(
  PI_Shapes,
  (pi_string,                     color)
  (int,                               x)
  (int,                               y)
  (int,                       shapesize)
  (pi_vector<int>,                 nums)  
  (pi_vector<pi_string>,             vs)
  (pi_vector<pi_vector<pi_string>>, vvs))

