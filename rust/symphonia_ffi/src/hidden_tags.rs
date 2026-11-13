// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Symphonia's MP4 reader parses the iTunes tags while it opens a file and gives up on the whole file when one is
//! malformed, such as a genre of one byte. CamelPlayer reads tags with TagLib, so the boxes holding them are shown to
//! Symphonia as free space; the file itself is never changed.

use std::io::{self, Read, Seek, SeekFrom};

use symphonia::core::io::MediaSource;

pub struct HiddenTags<R> {
    inner: R,
    len: u64,
    pos: u64,
    /// Offsets of the box types that read as "free".
    hidden: Vec<u64>,
}

impl<R: Read + Seek> HiddenTags<R> {
    pub fn new(mut inner: R) -> io::Result<Self> {
        let len = inner.seek(SeekFrom::End(0))?;
        let hidden = tag_boxes(&mut inner, len)?;
        inner.seek(SeekFrom::Start(0))?;
        Ok(Self {
            inner,
            len,
            pos: 0,
            hidden,
        })
    }
}

struct Mp4Box {
    kind: [u8; 4],
    start: u64,
    payload: u64,
    end: u64,
}

/// The boxes from `start` to `end`, up to the first one whose size does not fit.
fn boxes<R: Read + Seek>(r: &mut R, start: u64, end: u64) -> io::Result<Vec<Mp4Box>> {
    let mut out = Vec::new();
    let mut pos = start;
    while end - pos >= 8 {
        let mut h = [0u8; 16];
        r.seek(SeekFrom::Start(pos))?;
        r.read_exact(&mut h[..8])?;
        let mut size = u64::from(u32::from_be_bytes([h[0], h[1], h[2], h[3]]));
        let mut header = 8;
        if size == 1 {
            if end - pos < 16 {
                break;
            }
            r.read_exact(&mut h[8..])?;
            size = u64::from_be_bytes([h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]]);
            header = 16;
        } else if size == 0 {
            size = end - pos;
        }
        if size < header || size > end - pos {
            break;
        }
        out.push(Mp4Box {
            kind: [h[4], h[5], h[6], h[7]],
            start: pos,
            payload: pos + header,
            end: pos + size,
        });
        pos += size;
    }
    Ok(out)
}

/// The types of the boxes Symphonia reads tags from: a top-level meta and the movie's udta. None outside MP4 files.
fn tag_boxes<R: Read + Seek>(r: &mut R, len: u64) -> io::Result<Vec<u64>> {
    // The first type first: a FLAC or WAV header read as a box size passes for a large file, and walking on from it
    // would read far into the file, on a NAS too, before the result was dropped.
    let mut first = [0u8; 8];
    r.seek(SeekFrom::Start(0))?;
    if len < 8 || r.read_exact(&mut first).is_err() || &first[4..] != b"ftyp" {
        return Ok(Vec::new());
    }
    let top = boxes(r, 0, len)?;
    if top.first().map(|b| &b.kind) != Some(b"ftyp") {
        return Ok(Vec::new());
    }
    let mut hidden = Vec::new();
    for b in &top {
        match &b.kind {
            b"meta" => hidden.push(b.start + 4),
            b"moov" => hidden.extend(
                boxes(r, b.payload, b.end)?
                    .iter()
                    .filter(|c| &c.kind == b"udta")
                    .map(|c| c.start + 4),
            ),
            _ => {}
        }
    }
    Ok(hidden)
}

impl<R: Read> Read for HiddenTags<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        let end = self.pos + n as u64;
        for &at in &self.hidden {
            for (offset, byte) in (at..at + 4).zip(b"free") {
                if (self.pos..end).contains(&offset) {
                    buf[(offset - self.pos) as usize] = *byte;
                }
            }
        }
        self.pos = end;
        Ok(n)
    }
}

impl<R: Seek> Seek for HiddenTags<R> {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        self.pos = self.inner.seek(to)?;
        Ok(self.pos)
    }
}

impl<R: Read + Seek + Send + Sync> MediaSource for HiddenTags<R> {
    fn is_seekable(&self) -> bool {
        true
    }

    fn byte_len(&self) -> Option<u64> {
        Some(self.len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn mp4_box(kind: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut b = (8 + payload.len() as u32).to_be_bytes().to_vec();
        b.extend_from_slice(kind);
        b.extend_from_slice(payload);
        b
    }

    /// Reads all of `bytes` through HiddenTags three bytes at a time, so hidden types straddle reads.
    fn shown(bytes: &[u8]) -> Vec<u8> {
        let mut source = HiddenTags::new(Cursor::new(bytes.to_vec())).unwrap();
        let mut out = Vec::new();
        let mut chunk = [0u8; 3];
        loop {
            let n = source.read(&mut chunk).unwrap();
            if n == 0 {
                return out;
            }
            out.extend_from_slice(&chunk[..n]);
        }
    }

    #[test]
    fn tag_boxes_read_as_free_space() {
        let udta = mp4_box(b"udta", &mp4_box(b"meta", &[0; 12]));
        let moov = mp4_box(b"moov", &[mp4_box(b"mvhd", &[0; 100]), udta].concat());
        let mut mdat = 1u32.to_be_bytes().to_vec(); // a 64-bit size
        mdat.extend_from_slice(b"mdat");
        mdat.extend_from_slice(&24u64.to_be_bytes());
        mdat.extend_from_slice(&[0x55; 8]);
        let file = [
            mp4_box(b"ftyp", b"M4A \0\0\0\0"),
            mdat,
            moov,
            mp4_box(b"meta", &[1; 4]),
        ]
        .concat();

        let mut expected = file.clone();
        for kind in [&b"udta"[..], &b"meta"[..]] {
            let at = file.windows(4).rposition(|w| w == kind).unwrap();
            expected[at..at + 4].copy_from_slice(b"free");
        }
        assert_eq!(shown(&file), expected);

        let mut source = HiddenTags::new(Cursor::new(file.clone())).unwrap();
        let udta = file.windows(4).position(|w| w == b"udta").unwrap() as u64;
        source.seek(SeekFrom::Start(udta - 4)).unwrap();
        let mut header = [0u8; 8];
        source.read_exact(&mut header).unwrap();
        assert_eq!(&header[4..], b"free");
        assert_eq!(source.byte_len(), Some(file.len() as u64));
    }

    /// A file of `len` bytes that begins with `head`, zeros after it, noting how far any read reached.
    struct Large {
        head: Vec<u8>,
        len: u64,
        pos: u64,
        furthest: u64,
    }

    impl Read for Large {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let n = buf.len().min((self.len - self.pos.min(self.len)) as usize);
            for (i, byte) in buf[..n].iter_mut().enumerate() {
                let at = self.pos as usize + i;
                *byte = self.head.get(at).copied().unwrap_or(0);
            }
            self.pos += n as u64;
            if n > 0 {
                self.furthest = self.furthest.max(self.pos);
            }
            Ok(n)
        }
    }

    impl Seek for Large {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.pos = match to {
                SeekFrom::Start(at) => at,
                SeekFrom::End(back) => (self.len as i64 + back) as u64,
                SeekFrom::Current(by) => (self.pos as i64 + by) as u64,
            };
            Ok(self.pos)
        }
    }

    #[test]
    fn a_large_file_that_is_not_mp4_is_read_only_at_its_start() {
        // "RIFF" and "fLaC" read as box sizes of about 1.38 and 1.72 GB, which such files exceed.
        for head in [
            &b"RIFF\x24\0\0\0WAVEfmt "[..],
            &b"fLaC\0\0\0\x22\x10\0\x10\0"[..],
        ] {
            let mut file = Large {
                head: head.to_vec(),
                len: 2_000_000_000,
                pos: 0,
                furthest: 0,
            };
            assert!(tag_boxes(&mut file, 2_000_000_000).unwrap().is_empty());
            assert!(file.furthest <= 16, "read up to byte {}", file.furthest);
        }
    }

    #[test]
    fn other_files_pass_unchanged() {
        let flac = [b"fLaC".as_slice(), &[0, 0, 0, 34], b"udta", &[0; 30]].concat();
        assert_eq!(shown(&flac), flac);
        let truncated = [
            mp4_box(b"ftyp", b"M4A \0\0\0\0"),
            500u32.to_be_bytes().to_vec(),
            b"moov".to_vec(),
        ]
        .concat();
        assert_eq!(shown(&truncated), truncated);
    }
}
