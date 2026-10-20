// Native DPI scalar/vector and open-array interface.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char svBit;
typedef unsigned char svLogic;
typedef uint32_t svBitVecVal;
typedef struct {
  uint32_t aval, bval;
} svLogicVecVal;
typedef void *svOpenArrayHandle;
typedef void *svScope;
svScope svGetScope(void);
svScope svSetScope(const svScope);
const char *svGetNameFromScope(const svScope);
svScope svGetScopeFromName(const char *name);
int svPutUserData(const svScope, void *key, void *data);
void *svGetUserData(const svScope, void *key);
int svGetCallerInfo(const char **file, int *line);
#define sv_0 0
#define sv_1 1
#define sv_z 2
#define sv_x 3
#define SV_PACKED_DATA_NELEMS(WIDTH) (((WIDTH) + 31) >> 5)
svBit svGetBitselBit(const svBitVecVal *, int);
svLogic svGetBitselLogic(const svLogicVecVal *, int);
void svPutBitselBit(svBitVecVal *, int, svBit);
void svPutBitselLogic(svLogicVecVal *, int, svLogic);
void svGetPartselBit(svBitVecVal *, const svBitVecVal *, int, int);
void svGetPartselLogic(svLogicVecVal *, const svLogicVecVal *, int, int);
void svPutPartselBit(svBitVecVal *, svBitVecVal, int, int);
void svPutPartselLogic(svLogicVecVal *, svLogicVecVal, int, int);
int svDimensions(const svOpenArrayHandle);
int svSize(const svOpenArrayHandle, int dimension);
int svLeft(const svOpenArrayHandle, int dimension);
int svRight(const svOpenArrayHandle, int dimension);
int svLow(const svOpenArrayHandle, int dimension);
int svHigh(const svOpenArrayHandle, int dimension);
int svIncrement(const svOpenArrayHandle, int dimension);
int svSizeOfArray(const svOpenArrayHandle);
void *svGetArrayPtr(const svOpenArrayHandle);
void *svGetArrElemPtr(const svOpenArrayHandle, int, ...);
void *svGetArrElemPtr1(const svOpenArrayHandle, int);
void *svGetArrElemPtr2(const svOpenArrayHandle, int, int);
void *svGetArrElemPtr3(const svOpenArrayHandle, int, int, int);
svBit svGetBitArrElem(const svOpenArrayHandle, int, ...);
void svPutBitArrElem(const svOpenArrayHandle, svBit, int, ...);
void svGetBitArrElemVecVal(svBitVecVal *, const svOpenArrayHandle, int, ...);
void svPutBitArrElemVecVal(const svOpenArrayHandle, const svBitVecVal *, int,
                           ...);
svBit svGetBitArrElem1(const svOpenArrayHandle, int);
void svPutBitArrElem1(const svOpenArrayHandle, svBit, int);
void svGetBitArrElem1VecVal(svBitVecVal *, const svOpenArrayHandle, int);
void svPutBitArrElem1VecVal(const svOpenArrayHandle, const svBitVecVal *, int);
svBit svGetBitArrElem2(const svOpenArrayHandle, int, int);
void svPutBitArrElem2(const svOpenArrayHandle, svBit, int, int);
void svGetBitArrElem2VecVal(svBitVecVal *, const svOpenArrayHandle, int, int);
void svPutBitArrElem2VecVal(const svOpenArrayHandle, const svBitVecVal *, int,
                            int);
svBit svGetBitArrElem3(const svOpenArrayHandle, int, int, int);
void svPutBitArrElem3(const svOpenArrayHandle, svBit, int, int, int);
void svGetBitArrElem3VecVal(svBitVecVal *, const svOpenArrayHandle, int, int,
                            int);
void svPutBitArrElem3VecVal(const svOpenArrayHandle, const svBitVecVal *, int,
                            int, int);
svLogic svGetLogicArrElem(const svOpenArrayHandle, int, ...);
void svPutLogicArrElem(const svOpenArrayHandle, svLogic, int, ...);
void svGetLogicArrElemVecVal(svLogicVecVal *, const svOpenArrayHandle, int,
                             ...);
void svPutLogicArrElemVecVal(const svOpenArrayHandle, const svLogicVecVal *,
                             int, ...);
svLogic svGetLogicArrElem1(const svOpenArrayHandle, int);
void svPutLogicArrElem1(const svOpenArrayHandle, svLogic, int);
void svGetLogicArrElem1VecVal(svLogicVecVal *, const svOpenArrayHandle, int);
void svPutLogicArrElem1VecVal(const svOpenArrayHandle, const svLogicVecVal *,
                              int);
svLogic svGetLogicArrElem2(const svOpenArrayHandle, int, int);
void svPutLogicArrElem2(const svOpenArrayHandle, svLogic, int, int);
void svGetLogicArrElem2VecVal(svLogicVecVal *, const svOpenArrayHandle, int,
                              int);
void svPutLogicArrElem2VecVal(const svOpenArrayHandle, const svLogicVecVal *,
                              int, int);
svLogic svGetLogicArrElem3(const svOpenArrayHandle, int, int, int);
void svPutLogicArrElem3(const svOpenArrayHandle, svLogic, int, int, int);
void svGetLogicArrElem3VecVal(svLogicVecVal *, const svOpenArrayHandle, int,
                              int, int);
void svPutLogicArrElem3VecVal(const svOpenArrayHandle, const svLogicVecVal *,
                              int, int, int);
#ifdef __cplusplus
}
#endif
