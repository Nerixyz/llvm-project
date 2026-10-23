#include <cstddef>

struct Base1 {
  virtual ~Base1() = default;
  int base1 = 1;
};
struct Base2 {
  virtual ~Base2() = default;
  int base2 = 2;
};
struct VBase {
  virtual ~VBase() = default;
  int vbase = 5;
};

struct Single : public Base1 {
  int single = 3;
};
struct Multiple : public virtual VBase, public Base1, public Base2 {
  int multiple = 4;
};

struct VBaseUser1 : public virtual VBase {
  int vbase_user1 = 6;
};
struct VBaseUser2 : public virtual VBase {
  int vbase_user2 = 7;
};

struct VBaseDiamondUser : public VBaseUser1, public VBaseUser2 {
  int vbase_duo_user = 8;
  virtual void foo() {}
};

struct MultiWrap1 : public Multiple {
  int multi_wrap1 = 9;
};
struct MultiWrap2 : public Multiple {
  int multi_wrap2 = 10;
};

// With the Microsoft ABI, many of the vftable symbols here will have additional
// components to disambiguate.
struct Combined : public MultiWrap1,
                  public VBaseDiamondUser,
                  public MultiWrap2,
                  public Single {
  int combined = 11;

  virtual void something() {}
};

int main() {
  Single *single = new Single;
  Base1 *single_base1 = single;

  // --------------------------

  Multiple *multi = new Multiple;
  Base1 *multi_base1 = multi;
  Base2 *multi_base2 = multi;

  // --------------------------

  VBaseDiamondUser *vbuser = new VBaseDiamondUser;
  VBase *vbuser_vbase = vbuser;

  // --------------------------

  Combined *combined = new Combined;

  MultiWrap1 *combined_mw1 = combined;
  Multiple *combined_mw1_multi = combined_mw1;
  Base1 *combined_mw1_multi_b1 = combined_mw1_multi;
  Base2 *combined_mw1_multi_b2 = combined_mw1_multi;

  VBaseDiamondUser *combined_vbuser = combined;
  VBase *combined_vbuser_vbase = combined_vbuser;

  MultiWrap2 *combined_mw2 = combined;
  Multiple *combined_mw2_multi = combined_mw2;
  Base1 *combined_mw2_multi_b1 = combined_mw2_multi;
  Base2 *combined_mw2_multi_b2 = combined_mw2_multi;

  Single *combined_single = combined;
  Base1 *combined_single_base1 = combined_single;

  return 1; // break here
}
