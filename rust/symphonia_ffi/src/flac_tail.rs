// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Some rips carry bytes after the last FLAC frame that are neither audio nor a tag. Symphonia's reader takes them for
//! part of that frame, finds its CRC wrong and drops the frame, so the track ends early. The frame is found here in the
//! file's tail by its header; its end is one of the places where its CRC-16 checks, the first one it decodes to.

use std::io::{self, Read, Seek, SeekFrom};

const TAIL_BYTES: u64 = 1 << 20;

pub fn read_tail(path: &str) -> io::Result<Vec<u8>> {
    let mut file = std::fs::File::open(path)?;
    let len = file.seek(SeekFrom::End(0))?;
    file.seek(SeekFrom::Start(len.saturating_sub(TAIL_BYTES)))?;
    let mut tail = Vec::new();
    file.read_to_end(&mut tail)?;
    Ok(tail)
}

/// The byte ranges `tail` could hold the frame of `block` samples starting at sample `first` in: from a header that
/// says so to each place where the CRC-16 checks, the last header first and the shortest range first.
pub fn frame_candidates(
    tail: &[u8],
    first: u64,
    block: u64,
    max_block: u64,
) -> Vec<(usize, usize)> {
    let mut found = Vec::new();
    for at in (0..tail.len()).rev() {
        let Some((start, size, header_len)) = header(&tail[at..], max_block) else {
            continue;
        };
        if start != first || size != block {
            continue;
        }
        let mut crc = 0u16;
        for (i, &byte) in tail[at..].iter().enumerate() {
            crc = crc16(crc, byte);
            if crc == 0 && i + 1 >= header_len + 2 {
                found.push((at, at + i + 1));
            }
        }
    }
    found
}

/// The first sample, the block size and the header's length of a frame header at the start of `buf`.
fn header(buf: &[u8], max_block: u64) -> Option<(u64, u64, usize)> {
    if buf.len() < 6 || buf[0] != 0xFF || buf[1] & 0xFE != 0xF8 {
        return None;
    }
    let variable = buf[1] & 1 != 0;
    let (block_code, rate_code) = (buf[2] >> 4, buf[2] & 0x0F);
    if block_code == 0 || rate_code == 15 || buf[3] >> 4 > 10 || buf[3] & 1 != 0 {
        return None;
    }
    let lead = buf[4];
    let extra = match lead.leading_ones() {
        0 => 0,
        n @ 2..=7 => n as usize - 1,
        _ => return None,
    };
    let mut number = u64::from(lead & (0x7F >> extra));
    let mut pos = 5;
    for _ in 0..extra {
        let byte = *buf.get(pos)?;
        if byte & 0xC0 != 0x80 {
            return None;
        }
        number = number << 6 | u64::from(byte & 0x3F);
        pos += 1;
    }
    let block = match block_code {
        1 => 192,
        2..=5 => 576 << (block_code - 2),
        6 => {
            pos += 1;
            u64::from(*buf.get(pos - 1)?) + 1
        }
        7 => {
            pos += 2;
            u64::from(u16::from_be_bytes([*buf.get(pos - 2)?, *buf.get(pos - 1)?])) + 1
        }
        _ => 256 << (block_code - 8),
    };
    pos += match rate_code {
        12 => 1,
        13 | 14 => 2,
        _ => 0,
    };
    if buf.get(..pos)?.iter().fold(0u8, |crc, &b| crc8(crc, b)) != *buf.get(pos)? {
        return None;
    }
    let first = if variable { number } else { number * max_block };
    Some((first, block, pos + 1))
}

fn crc8(crc: u8, byte: u8) -> u8 {
    let mut crc = crc ^ byte;
    for _ in 0..8 {
        crc = if crc & 0x80 != 0 {
            crc << 1 ^ 0x07
        } else {
            crc << 1
        };
    }
    crc
}

fn crc16(crc: u16, byte: u8) -> u16 {
    let mut crc = crc ^ (u16::from(byte) << 8);
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 {
            crc << 1 ^ 0x8005
        } else {
            crc << 1
        };
    }
    crc
}
