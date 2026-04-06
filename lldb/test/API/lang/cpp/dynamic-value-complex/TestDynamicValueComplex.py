"""
Test that we can resolve dynamic values in complex inheritance hierarchies.
This is mainly relevant for the Microsoft ABI where there can be multiple vtables in one type.
"""

import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


class DynamicValueComplexTestCase(TestBase):
    TEST_WITH_PDB_DEBUG_INFO = True
    SHARED_BUILD_TESTCASE = False

    @skipIf(oslist=["windows"], debug_info=["dwarf"], bugnumber="llvm.org/pr24663")
    def test_rtti(self):
        """Test with RTTI"""
        self.build(dictionary={"CXXFLAGS_EXTRAS": ""})
        self.do_test(True)

    @skipIf(oslist=["windows"], debug_info=["dwarf"], bugnumber="llvm.org/pr24663")
    def test_no_rtti(self):
        """Test without RTTI"""
        self.build(dictionary={"CXXFLAGS_EXTRAS": "-fno-rtti"})
        self.do_test(False)

    def do_test(self, has_rtti: bool):
        (target, process, thread, bkpt) = lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.cpp")
        )

        frame = thread.frame[0]
        get = lambda name: self._check_get(frame, name)

        # Test Single.
        single = get("single")
        single_base1 = get("single_base1")
        single_addr = single.GetValueAsAddress()
        self.assertNotEqual(single_addr, lldb.LLDB_INVALID_ADDRESS)
        self.assertEqual(single_addr, single_base1.GetValueAsAddress())
        self.assertEqual(single.GetTypeName(), single_base1.GetTypeName())

        single_children = [
            ValueCheck(
                type="Base1",
                children=[ValueCheck(type="int", name="base1", value="1")],
            ),
            ValueCheck(type="int", name="single", value="3"),
        ]
        self.expect_expr(
            "single", result_type="Single *", result_children=single_children
        )

        # Test Multiple.
        multi = get("multi")
        multi_base1 = get("multi_base1")
        multi_base2 = get("multi_base2")
        multi_addr = multi.GetValueAsAddress()
        self.assertNotEqual(multi_addr, lldb.LLDB_INVALID_ADDRESS)
        self.assertEqual(multi_addr, multi_base1.GetValueAsAddress())
        self.assertEqual(multi_addr, multi_base2.GetValueAsAddress())
        self.assertEqual(multi.GetTypeName(), multi_base1.GetTypeName())
        self.assertEqual(multi.GetTypeName(), multi_base2.GetTypeName())

        multi_children = [
            ValueCheck(
                type="Base1",
                children=[ValueCheck(type="int", name="base1", value="1")],
            ),
            ValueCheck(
                type="Base2",
                children=[ValueCheck(type="int", name="base2", value="2")],
            ),
            ValueCheck(
                type="VBase",
                children=[ValueCheck(type="int", name="vbase", value="5")],
            ),
            ValueCheck(type="int", name="multiple", value="4"),
        ]
        self.expect_expr(
            "multi", result_type="Multiple *", result_children=multi_children
        )

        # Test VBaseDiamondUser.
        vbuser = get("vbuser")
        vbuser_vbase = get("vbuser_vbase")
        vbuser_addr = vbuser.GetValueAsAddress()
        if (
            self.getPlatform() == "windows"
            and self.getDebugInfo() == "pdb"
            and not has_rtti
        ):
            # On Windows with PDB, we have to use the AST to get the offset.
            # But we don't know where the virtual base will be located in the object.
            self.assertNotEqual(vbuser_addr, lldb.LLDB_INVALID_ADDRESS)
            self.assertNotEqual(
                vbuser_vbase.GetValueAsAddress(), lldb.LLDB_INVALID_ADDRESS
            )
            self.assertNotEqual(vbuser_addr, vbuser_vbase.GetValueAsAddress())
            self.assertNotEqual(vbuser.GetTypeName(), vbuser_vbase.GetTypeName())
        else:
            self.assertNotEqual(vbuser_addr, lldb.LLDB_INVALID_ADDRESS)
            self.assertEqual(vbuser_addr, vbuser_vbase.GetValueAsAddress())
            self.assertEqual(vbuser.GetTypeName(), vbuser_vbase.GetTypeName())

        vbuser_children = [
            ValueCheck(
                type="VBaseUser1",
                children=[
                    ValueCheck(
                        type="VBase",
                        children=[ValueCheck(type="int", name="vbase", value="5")],
                    ),
                    ValueCheck(type="int", name="vbase_user1", value="6"),
                ],
            ),
            ValueCheck(
                type="VBaseUser2",
                children=[
                    ValueCheck(
                        type="VBase",
                        children=[ValueCheck(type="int", name="vbase", value="5")],
                    ),
                    ValueCheck(type="int", name="vbase_user2", value="7"),
                ],
            ),
            ValueCheck(type="int", name="vbase_duo_user", value="8"),
        ]
        self.expect_expr(
            "vbuser", result_type="VBaseDiamondUser *", result_children=vbuser_children
        )

        # Test Combined.
        combined = get("combined")
        combined_addr = combined.GetValueAsAddress()
        combined_name = combined.GetTypeName()
        self.assertNotEqual(combined_addr, lldb.LLDB_INVALID_ADDRESS)
        self.assertGreater(len(combined_name), 0)

        combined_mw1 = get("combined_mw1")
        self.assertEqual(combined_addr, combined_mw1.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw1.GetTypeName())
        combined_mw1_multi = get("combined_mw1_multi")
        self.assertEqual(combined_addr, combined_mw1_multi.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw1_multi.GetTypeName())
        combined_mw1_multi_b1 = get("combined_mw1_multi_b1")
        self.assertEqual(combined_addr, combined_mw1_multi_b1.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw1_multi_b1.GetTypeName())
        combined_mw1_multi_b2 = get("combined_mw1_multi_b2")
        self.assertEqual(combined_addr, combined_mw1_multi_b2.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw1_multi_b2.GetTypeName())

        combined_vbuser = get("combined_vbuser")
        self.assertEqual(combined_addr, combined_vbuser.GetValueAsAddress())
        self.assertEqual(combined_name, combined_vbuser.GetTypeName())

        combined_vbuser_vbase = get("combined_vbuser_vbase")
        if (
            self.getPlatform() == "windows"
            and self.getDebugInfo() == "pdb"
            and not has_rtti
        ):
            vbuser_vbase_addr = combined_vbuser_vbase.GetValueAsAddress()
            self.assertNotEqual(vbuser_vbase_addr, lldb.LLDB_INVALID_ADDRESS)
            self.assertNotEqual(combined_addr, vbuser_vbase_addr)
            self.assertNotEqual(combined_name, combined_vbuser_vbase.GetTypeName())
        else:
            self.assertEqual(combined_addr, combined_vbuser_vbase.GetValueAsAddress())
            self.assertEqual(combined_name, combined_vbuser_vbase.GetTypeName())

        combined_mw2 = get("combined_mw2")
        self.assertEqual(combined_addr, combined_mw2.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw2.GetTypeName())
        combined_mw2_multi = get("combined_mw2_multi")
        self.assertEqual(combined_addr, combined_mw2_multi.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw2_multi.GetTypeName())
        combined_mw2_multi_b1 = get("combined_mw2_multi_b1")
        self.assertEqual(combined_addr, combined_mw2_multi_b1.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw2_multi_b1.GetTypeName())
        combined_mw2_multi_b2 = get("combined_mw2_multi_b2")
        self.assertEqual(combined_addr, combined_mw2_multi_b2.GetValueAsAddress())
        self.assertEqual(combined_name, combined_mw2_multi_b2.GetTypeName())

        combined_single = get("combined_single")
        self.assertEqual(combined_addr, combined_single.GetValueAsAddress())
        self.assertEqual(combined_name, combined_single.GetTypeName())
        combined_single_base1 = get("combined_single_base1")
        self.assertEqual(combined_addr, combined_single_base1.GetValueAsAddress())
        self.assertEqual(combined_name, combined_single_base1.GetTypeName())

        combined_children = [
            ValueCheck(
                type="MultiWrap1",
                children=[
                    ValueCheck(type="Multiple", children=multi_children),
                    ValueCheck(type="int", name="multi_wrap1", value="9"),
                ],
            ),
            ValueCheck(type="VBaseDiamondUser", children=vbuser_children),
            ValueCheck(
                type="MultiWrap2",
                children=[
                    ValueCheck(type="Multiple", children=multi_children),
                    ValueCheck(type="int", name="multi_wrap2", value="10"),
                ],
            ),
            ValueCheck(type="Single", children=single_children),
            ValueCheck(type="int", name="combined", value="11"),
        ]
        self.expect_expr(
            "combined", result_type="Combined *", result_children=combined_children
        )

    def _check_get(self, frame: lldb.SBFrame, name: str) -> lldb.SBValue:
        var = frame.FindVariable(name, lldb.eDynamicCanRunTarget)
        self.assertTrue(var, f"could not find '{name}'")
        return var
