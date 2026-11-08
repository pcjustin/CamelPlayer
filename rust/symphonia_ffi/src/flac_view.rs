// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Symphonia reads every metadata block of a FLAC while it opens the file, a cover of a few megabytes included, and from
//! a NAS at about 2 MB/s a 3.3 MB cover delayed the start of playback by 1.5 s. Symphonia sees the file through this
//! view: the metadata rebuilt without its PICTURE and PADDING blocks, then the file from its first audio frame on. Seek
//! tables count from the first frame, so seeking is unchanged. Other files are passed through; the file itself is never
//! changed.

use std::io::{self, Read, Seek, SeekFrom};

use symphonia::core::io::MediaSource;

const PADDING: u8 = 1;
const PICTURE: u8 = 6;
const LAST: u8 = 0x80;

pub struct FlacView<R> {
    inner: R,
    /// The rebuilt metadata, empty when the file is passed through.
    header: Vec<u8>,
    /// Where the audio starts in the file, shown at `header.len()`.
    body: u64,
    len: u64,
    pos: u64,
}

impl<R: Read + Seek> FlacView<R> {
    pub fn new(mut inner: R) -> io::Result<Self> {
        let file_len = inner.seek(SeekFrom::End(0))?;
        let kept = kept_metadata(&mut inner, file_len)?;
        inner.seek(SeekFrom::Start(0))?;
        let (header, body) = kept.unwrap_or_default();
        Ok(Self {
            inner,
            len: header.len() as u64 + file_len - body,
            header,
            body,
            pos: 0,
        })
    }
}

/// The FLAC marker and the metadata blocks other than pictures and padding, with the audio's offset; None when there is
/// nothing to leave out or the file is not a FLAC whose metadata reads whole.
fn kept_metadata<R: Read + Seek>(r: &mut R, len: u64) -> io::Result<Option<(Vec<u8>, u64)>> {
    let mut marker = [0u8; 4];
    r.seek(SeekFrom::Start(0))?;
    if len < 8 || r.read_exact(&mut marker).is_err() || &marker != b"fLaC" {
        return Ok(None);
    }
    let mut header = b"fLaC".to_vec();
    let mut last_kept = None;
    let mut dropped = false;
    let mut pos = 4u64;
    loop {
        let mut h = [0u8; 4];
        if len - pos < 4 {
            return Ok(None);
        }
        r.seek(SeekFrom::Start(pos))?;
        r.read_exact(&mut h)?;
        let size = u64::from(u32::from_be_bytes([0, h[1], h[2], h[3]]));
        if size > len - pos - 4 {
            return Ok(None);
        }
        let kind = h[0] & !LAST;
        if kind == PICTURE || kind == PADDING {
            dropped = true;
        } else {
            last_kept = Some(header.len());
            header.extend_from_slice(&[kind, h[1], h[2], h[3]]);
            let start = header.len();
            header.resize(start + size as usize, 0);
            r.read_exact(&mut header[start..])?;
        }
        pos += 4 + size;
        if h[0] & LAST != 0 {
            break;
        }
    }
    match last_kept {
        Some(at) if dropped => {
            header[at] |= LAST;
            Ok(Some((header, pos)))
        }
        _ => Ok(None),
    }
}

impl<R: Read + Seek> Read for FlacView<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let shown = self.header.len() as u64;
        if self.pos < shown {
            let from = &self.header[self.pos as usize..];
            let n = from.len().min(buf.len());
            buf[..n].copy_from_slice(&from[..n]);
            self.pos += n as u64;
            return Ok(n);
        }
        self.inner
            .seek(SeekFrom::Start(self.body + self.pos - shown))?;
        let n = self.inner.read(buf)?;
        self.pos += n as u64;
        Ok(n)
    }
}

impl<R: Seek> Seek for FlacView<R> {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let target = match to {
            SeekFrom::Start(at) => Some(at),
            SeekFrom::End(delta) => self.len.checked_add_signed(delta),
            SeekFrom::Current(delta) => self.pos.checked_add_signed(delta),
        };
        self.pos = target
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "seek before the start"))?;
        Ok(self.pos)
    }
}

impl<R: Read + Seek + Send + Sync> MediaSource for FlacView<R> {
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

    fn block(kind: u8, payload: &[u8]) -> Vec<u8> {
        let size = (payload.len() as u32).to_be_bytes();
        [&[kind, size[1], size[2], size[3]][..], payload].concat()
    }

    fn all(view: &mut FlacView<Cursor<Vec<u8>>>) -> Vec<u8> {
        let mut out = Vec::new();
        let mut chunk = [0u8; 5];
        loop {
            let n = view.read(&mut chunk).unwrap();
            if n == 0 {
                return out;
            }
            out.extend_from_slice(&chunk[..n]);
        }
    }

    #[test]
    fn pictures_and_padding_are_left_out() {
        let audio = [0xFFu8, 0xF8, 1, 2, 3, 4, 5, 6, 7];
        let file = [
            b"fLaC".to_vec(),
            block(0, &[7; 34]),
            block(4, b"comment"),
            block(PICTURE, &[9; 1000]),
            block(3, &[8; 18]),
            block(PADDING | LAST, &[0; 500]),
            audio.to_vec(),
        ]
        .concat();
        let mut view = FlacView::new(Cursor::new(file)).unwrap();
        let expected = [
            b"fLaC".to_vec(),
            block(0, &[7; 34]),
            block(4, b"comment"),
            block(3 | LAST, &[8; 18]),
            audio.to_vec(),
        ]
        .concat();
        assert_eq!(view.byte_len(), Some(expected.len() as u64));
        assert_eq!(all(&mut view), expected);

        let at = view.seek(SeekFrom::End(-3)).unwrap();
        assert_eq!(at, expected.len() as u64 - 3);
        let mut tail = [0u8; 3];
        view.read_exact(&mut tail).unwrap();
        assert_eq!(tail, [5, 6, 7]);
        view.seek(SeekFrom::Start(2)).unwrap();
        view.seek(SeekFrom::Current(2)).unwrap();
        let mut first = [0u8; 4];
        view.read_exact(&mut first).unwrap();
        assert_eq!(first, [0, 0, 0, 34]);
    }

    #[test]
    fn other_files_pass_unchanged() {
        let plain = [b"fLaC".to_vec(), block(LAST, &[7; 34]), vec![0xFF, 0xF8, 1]].concat();
        let truncated = [
            b"fLaC".to_vec(),
            block(PICTURE, &[9; 10]),
            block(0, &[7; 3]),
        ]
        .concat();
        for file in [plain, truncated, b"RIFF\0\0\0\0WAVE".to_vec()] {
            let mut view = FlacView::new(Cursor::new(file.clone())).unwrap();
            assert_eq!(all(&mut view), file);
        }
    }
}
