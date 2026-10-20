; RUN: llc -mtriple=riscv64 -mattr=+v -riscv-enable-vrf-bank-ra \
; RUN:   -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=riscv64 -mattr=+v -riscv-enable-vrf-bank-ra \
; RUN:   -riscv-vrf-bank-ra-algorithm=offline -verify-machineinstrs < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=riscv64 -mattr=+v -riscv-enable-vrf-bank-ra \
; RUN:   -riscv-vrf-bank-ra-algorithm=online -verify-machineinstrs < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=riscv64 -mattr=+v -verify-machineinstrs < %s | FileCheck %s

; Smoke test: enabling bank-aware RA must still produce a legal vadd.vv.
; Calling-convention arguments already sit in consecutive VRs (different banks).
define <vscale x 2 x i32> @vadd_vv(<vscale x 2 x i32> %a, <vscale x 2 x i32> %b, i64 %vl) {
; CHECK-LABEL: vadd_vv:
; CHECK:       vsetvli zero, a0, e32, m1, ta, ma
; CHECK-NEXT:    vadd.vv v8, v8, v9
; CHECK-NEXT:    ret
  %r = call <vscale x 2 x i32> @llvm.riscv.vadd.nxv2i32.nxv2i32(
      <vscale x 2 x i32> poison, <vscale x 2 x i32> %a, <vscale x 2 x i32> %b, i64 %vl)
  ret <vscale x 2 x i32> %r
}

; Tied MAC: dest may share a bank with a source; the two vector sources must
; still assemble.
define <vscale x 2 x i32> @vmacc_vv(<vscale x 2 x i32> %acc, <vscale x 2 x i32> %a, <vscale x 2 x i32> %b, i64 %vl) {
; CHECK-LABEL: vmacc_vv:
; CHECK:       vsetvli
; CHECK:       vmacc.vv
; CHECK:       ret
  %r = call <vscale x 2 x i32> @llvm.riscv.vmacc.nxv2i32.nxv2i32(
      <vscale x 2 x i32> %acc, <vscale x 2 x i32> %a, <vscale x 2 x i32> %b, i64 %vl, i64 0)
  ret <vscale x 2 x i32> %r
}

declare <vscale x 2 x i32> @llvm.riscv.vadd.nxv2i32.nxv2i32(<vscale x 2 x i32>, <vscale x 2 x i32>, <vscale x 2 x i32>, i64)
declare <vscale x 2 x i32> @llvm.riscv.vmacc.nxv2i32.nxv2i32(<vscale x 2 x i32>, <vscale x 2 x i32>, <vscale x 2 x i32>, i64, i64)
