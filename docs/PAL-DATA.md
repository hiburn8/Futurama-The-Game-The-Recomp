# PAL/EU data binding (#31)

Accepted retail image: title 0x46490002, region 4, timestamp 0x3F01BB06.
SHA-1: 58f76de5bc739bf016d74c1d7f23e348f65ea4b4.
SHA-256: 595f30cff2bd4a1c85fa007a4d67acb39aff2d0f0031b2f5fae2d5af49993532.
Verified locally against the owner's extracted disc on 2026-10-29. No bytes from it
are embedded in source or synthetic fixtures.

XbeImage translates only the explicit logical table ranges used by the native game.
Pointers read from PAL tables already point into PAL sections: String reads them
without a second translation. Unsupported table accesses fail instead of fabricating
zero defaults. NTSC-U behavior is preserved.

| Logical NTSC table | PAL table | PAL evidence |
|---|---|---|
| 0x158864 format classes | 0x1587D4 | Address operand in format helper 0x158790; old EU analysis identifies its bits-per-pixel semantics |
| 0x2427C8 D3D TSS defaults | 0x242728 | Address operand at 0x239645 |
| 0x2427E8 D3D RS defaults | 0x242748 | Address operand at 0x2395E7 |
| 0x2936A8 deferred dirty flags | 0x293608 | Old EU d3d.md documents access for state 0x5C..0x87 (effective table starts at 0x293778) |
| 0x2D60D8 cheat pointers | 0x2D6038 | Address operand at 0x2C7D1 |
| 0x2D615C level-text pairs | 0x2D60BC | Address operands at 0x2C570 and 0x2C578 |
| 0x2D6228 animation pointers | 0x2D6188 | Address operands at 0x3DE50, 0x3DF24, 0x3F2A3; index 0x66 resolves to death_fall as independently documented in old EU g8_cheats.md |
| 0x2D8358 Ni TSS defaults | 0x2D82B8 | Address operands at 0x158BE2 and 0x158BFE |
| 0x2D83D8 Ni RS defaults | 0x2D8338 | Address operand at 0x159424 |

The old EU analysis is the pinned FuturamaPC mirror ef6185988b3ad65f32bebcc8e90e3e7515c0c300.
The format table shifts by 0x90; the other listed tables shift by 0xA0.
This is not permission to translate executable addresses or arbitrary pointer data.

## Audio binding

These PAL addresses are independently referenced by the accepted image. The
following locations identify address operands (not complete instruction starts).
Only the listed table ranges are translated; no general relocation is applied.

| Logical NTSC table | PAL table | PAL address operand |
|---|---|---|
| 0x262960 HRTF index | 0x2628C0 | 0x249E53 |
| 0x2513A0 HRTF filters | 0x251300 | 0x249E64, 0x249E72 |
| 0x263A38 default filter | 0x263998 | Pointer entries 0x2639B8, 0x2639BC |
| 0x2510C0 centre gain | 0x251020 | 0x249C2D, 0x249C36 |
| 0x251178 centre cutoff | 0x2510D8 | 0x249C45, 0x249C4C |
| 0x263A68 reverb rows | 0x2639C8 | 0x24E39C |
| 0x263AB8 reverb signs | 0x263A18 | 0x24E3C5, 0x24E4B2 |
| 0x263AE8 reverb delays | 0x263A48 | 0x24E343 |
| 0x263BE0 GP executive | 0x263B40 | 0x24A0A9 |
| 0x3175D8 EP stereo module | 0x317538 | 0x250BF2 |
| 0x3172E8 EP mono module | 0x317248 | 0x250C06 |

EP also checks the module identifiers and decodes the expected coefficient-write
instructions. The GP loader validates DSP words and the effects image. All audio
entry points validate the accepted XBE identity before reading its tables.

## Movie binding

The accepted PAL XMV section starts at 0x264360 instead of 0x264400, with the
same 0x27B6C virtual extent and 0x27B5C raw extent. The decoder entry prologue
is checked. Logical decoder constants address section-relative offsets, while
pointers loaded from run/level descriptors use the actual PAL section base.
The two APIs are deliberately separate: the address ranges overlap, so guessing
which kind of pointer a caller supplied would be ambiguous.

The descriptor sets at logical 0x2747B0 and 0x274820 resolve to PAL 0x274710 and
0x274780. Their PAL address operands occur at 0x26615F/0x266354/0x266F3A and
0x266172/0x26635A/0x266F40 respectively. The owned-movie test checks all physical
pointers in both sets, decodes the first packet's frames and rejects an altered
image. This is a decoder smoke check, not visual equivalence or complete playback.

The common movie service uses SDL clock/sleep and the shared Xbox sound backend
on Linux. The VP/GP/EP mixer now feeds an SDL audio stream. Audible output, full movie playback, PAL
gameplay, localization, timing and save compatibility remain separate checks.
