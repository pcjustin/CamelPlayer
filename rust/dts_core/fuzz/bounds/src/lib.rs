// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! LLVM's coverage on Windows expects these bounds from compiler-rt, which rustc does not link there. The linker sorts
//! `.SCOV$CM` and `.SCOVP$M`, where the counters and their table go, between them. LLVM takes each start 8 bytes on. A
//! crate of its own, with no function to instrument: in a module that gets coverage, LLVM would add symbols of these
//! names and rename the ones below.

#[used]
#[unsafe(export_name = "__start___sancov_cntrs")]
#[unsafe(link_section = ".SCOV$CA")]
static mut COUNTERS_START: u64 = 0;

#[used]
#[unsafe(export_name = "__stop___sancov_cntrs")]
#[unsafe(link_section = ".SCOV$CZ")]
static mut COUNTERS_STOP: [u8; 0] = [];

#[used]
#[unsafe(export_name = "__start___sancov_pcs")]
#[unsafe(link_section = ".SCOVP$A")]
static PCS_START: u64 = 0;

#[used]
#[unsafe(export_name = "__stop___sancov_pcs")]
#[unsafe(link_section = ".SCOVP$Z")]
static PCS_STOP: u64 = 0;
