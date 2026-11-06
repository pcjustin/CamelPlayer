# Notice

The modules of this crate, except `src/stream.rs` and the crate root's first lines, are copied from
[oxideav-dts](https://github.com/OxideAV/oxideav-dts) at commit `ec55712` (0.0.2), MIT license, copyright
Karpelès Lab Inc.; its license is in `LICENSE-oxideav-dts`.

Left out: the encoder and what only it uses (`encoder.rs`, `bitwriter.rs`, `qmf_analysis.rs`, `lfe_analysis.rs`) and the
`oxideav-core` integration (`registry.rs`). Fixtures in `tests/fixtures` come from the same repository.
