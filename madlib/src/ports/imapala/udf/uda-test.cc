// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <gtest/gtest.h>
#include "udf/uda-test-harness.h"
//#include "util/logging.h"

using namespace impala;
using namespace impala_udf;
using namespace std;

//-------------------------------- Count ------------------------------------
// Example of implementing Count(int_col).
// The input type is: int
// The intermediate type is bigint
// the return type is bigint
void CountInit(UdfContext* context, BigIntVal* val) {
  val->is_null = false;
  val->val = 0;
}

void CountUpdate(UdfContext* context, const IntVal& input, BigIntVal* val) {
  // BigIntVal is the same ptr as what was passed to CountInit
  if (input.is_null) return;
  ++val->val;
}

void CountMerge(UdfContext* context, const BigIntVal& src, BigIntVal* dst) {
  dst->val += src.val;
}

BigIntVal CountFinalize(UdfContext* context, const BigIntVal& val) {
  return val;
}

//-------------------------------- Min(String) ------------------------------------
// Example of implementing MIN for strings.
// The input type is: STRING
// The intermediate type is BufferVal
// the return type is STRING
// This is a little more sophisticated since the result buffers are reused (it grows
// to the longest result string).
struct MinState {
  uint8_t* value;
  int len;
  int buffer_len;

  void Set(UdfContext* context, const StringVal& val) {
    if (buffer_len < val.len) {
      context->Free(value);
      value = context->Allocate(val.len);
      buffer_len = val.len;
    }
    memcpy(value, val.ptr, val.len);
    len = val.len;
  }
};

// Initialize the MinState scratch space
void MinInit(UdfContext* context, BufferVal* val) {
  MinState* state = reinterpret_cast<MinState*>(*val);
  state->value = NULL;
  state->buffer_len = 0;
}

// Update the min value, comparing with the current value in MinState
void MinUpdate(UdfContext* context, const StringVal& input, BufferVal* val) {
  if (input.is_null) return;
  MinState* state = reinterpret_cast<MinState*>(*val);
  if (state->value == NULL) {
    state->Set(context, input);
    return;
  }
  int cmp = memcmp(input.ptr, state->value, ::min(input.len, state->len));
  if (cmp < 0 || (cmp == 0 && input.len < state->len)) {
    state->Set(context, input);
  }
}

// Serialize the state into the min string
const BufferVal MinSerialize(UdfContext* context, const BufferVal& intermediate) {
  return intermediate;
}

// Merge is the same as Update since the serialized format is the raw input format
void MinMerge(UdfContext* context, const BufferVal& src, BufferVal* dst) {
  const MinState* src_state = reinterpret_cast<const MinState*>(src);
  if (src_state->value == NULL) return;
  MinUpdate(context, StringVal(src_state->value, src_state->len), dst);
}

// Finalize also just returns the string so is the same as MinSerialize.
StringVal MinFinalize(UdfContext* context, const BufferVal& val) {
  const MinState* state = reinterpret_cast<const MinState*>(val);
  if (state->value == NULL) return StringVal::null();
  StringVal result = StringVal(context, state->len);
  memcpy(result.ptr, state->value, state->len);
  return result;
}

//----------------------------- Bits after Xor ------------------------------------
// Example of a UDA that xors all the input bits and then returns the number of
// resulting bits that are set. This illustrates where the result and intermediate
// are the same type, but a transformation is still needed in Finialize()
// The input type is: double
// The intermediate type is bigint
// the return type is bigint
void XorInit(UdfContext* context, BigIntVal* val) {
  val->is_null = false;
  val->val = 0;
}

void XorUpdate(UdfContext* context, const double* input, BigIntVal* val) {
  // BigIntVal is the same ptr as what was passed to CountInit
  if (input == NULL) return;
  val->val |= *reinterpret_cast<const int64_t*>(input);
}

void XorMerge(UdfContext* context, const BigIntVal& src, BigIntVal* dst) {
  dst->val |= src.val;
}

BigIntVal XorFinalize(UdfContext* context, const BigIntVal& val) {
  int64_t set_bits = 0;
  // Do popcnt on val
  // set_bits = popcnt(val.val);
  return BigIntVal(set_bits);
}

//--------------------------- HLL(Distinct Estimate) ---------------------------------
// Example of implementing distinct estimate. As an example, we will compress the
// intermediate buffer.
// Note: this is not the actual algorithm but a sketch of how it would be implemented
// with the UDA interface.
// The input type is: bigint
// The intermediate type is string (fixed at 256 bytes)
// the return type is bigint
void DistinctEstimateInit(UdfContext* context, StringVal* val) {
  // Since this is known, this will be allocated to 256 bytes.
  assert(val->len == 256);
  memset(val->ptr, 0, 256);
}

void DistinctEstimatUpdate(UdfContext* context, const int64_t* input, StringVal* val) {
  if (input == NULL) return;
  for (int i = 0; i < 256; ++i) {
    int hash = 0;
    // Hash(input) with the ith hash function
    // hash = Hash(*input, i);
    val->ptr[i] = hash;
  }
}

StringVal DistinctEstimatSerialize(UdfContext* context, const StringVal& intermediate) {
  int compressed_size = 0;
  uint8_t* result = NULL; // SnappyCompress(intermediate.ptr, intermediate.len);
  return StringVal(result, compressed_size);
}

void DistinctEstimateMerge(UdfContext* context, const StringVal& src, StringVal* dst) {
  uint8_t* src_uncompressed = NULL; // SnappyUncompress(src.ptr, src.len);
  for (int i = 0; i < 256; ++i) {
    dst->ptr[i] ^= src_uncompressed[i];
  }
}

BigIntVal DistinctEstimateFinalize(UdfContext* context, const StringVal& val) {
  int64_t set_bits = 0;
  // Do popcnt on val
  // set_bits = popcnt(val.val);
  return BigIntVal(set_bits);
}

TEST(CountTest, Basic) {
  UdaTestHarness<IntVal, BigIntVal, BigIntVal> test1(
      CountInit, CountUpdate, CountMerge, NULL, CountFinalize);
  vector<IntVal> no_nulls;
  no_nulls.resize(1);

  EXPECT_TRUE(test1.Execute(no_nulls, BigIntVal(no_nulls.size())));
  EXPECT_FALSE(test1.Execute(no_nulls, BigIntVal(100)));
}

TEST(MinTest, Basic) {
  UdaTestHarness<StringVal, BufferVal, StringVal> test(
      MinInit, MinUpdate, MinMerge, MinSerialize, MinFinalize);
  test.SetIntermediateSize(sizeof(MinState));

  vector<StringVal> values;
  values.push_back(StringVal("BBB"));
  EXPECT_TRUE(test.Execute(values, StringVal("BBB")));

  values.push_back(StringVal("AA"));
  EXPECT_TRUE(test.Execute(values, StringVal("AA")));

  values.push_back(StringVal("CCC"));
  EXPECT_TRUE(test.Execute(values, StringVal("AA")));

  values.push_back(StringVal("ABCDEF"));
  values.push_back(StringVal("AABCDEF"));
  values.push_back(StringVal("A"));
  EXPECT_TRUE(test.Execute(values, StringVal("A"))) << test.GetErrorMsg();

  values.clear();
  values.push_back(StringVal::null());
  EXPECT_TRUE(test.Execute(values, StringVal::null())) << test.GetErrorMsg();

  values.push_back(StringVal("ZZZ"));
  EXPECT_TRUE(test.Execute(values, StringVal("ZZZ"))) << test.GetErrorMsg();
}

int main(int argc, char** argv) {
  //impala::InitGoogleLoggingSafe(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
