# Licensing

This repository is a fork of [Haivision SRT](https://github.com/Haivision/srt).
It contains code under **two different licenses**. Read this file before
redistributing.

| Part                               | Files                                          | License                                                |
|------------------------------------|------------------------------------------------|--------------------------------------------------------|
| Base SRT / libsrt (Haivision)      | everything except the SRTLA extension          | **MPL-2.0** — see [`LICENSE`](LICENSE)                 |
| SRTLA receiver extension (OpenIRL) | `srtcore/srtla_rec.cpp`, `srtcore/srtla_rec.h` | **AGPL-3.0-only** — see [`LICENSE.AGPL`](LICENSE.AGPL) |

The base license is unchanged: every original SRT/libsrt file remains MPL-2.0.

## The SRTLA receiver extension is licensed separately

The SRTLA receiver demux is an original implementation authored by **OpenIRL**:

> Copyright (c) 2026 OpenIRL
> SPDX-License-Identifier: **AGPL-3.0-only**

You may use, modify and distribute these files under the terms of the **GNU
Affero General Public License, version 3.0 only**, the full text of which is in
[`LICENSE.AGPL`](LICENSE.AGPL) (canonical copy:
<https://www.gnu.org/licenses/agpl-3.0.txt>). In particular, AGPL §13 requires
that if you run a modified version to provide a service over a network, you must
offer the complete corresponding source of your version to the users of that
service — i.e. **modifications must be made public**.

## How the two licenses interact in a build

MPL-2.0 is *file-level* copyleft: it binds only the individual MPL files. The
Haivision libsrt files here carry the standard MPL-2.0 notice **without** the
"Incompatible With Secondary Licenses" marker, so MPL-2.0 permits combining them
with AGPL code and distributing the resulting *Larger Work* under the AGPL.

Consequences:

- **Base libsrt stays free.** Anyone may take the MPL-2.0 files and build/use
  libsrt **without** the `srtla_rec.*` extension under MPL-2.0 terms, including
  commercially.
- **The SRTLA feature is the AGPL-gated part.** Any build or distribution that
  *includes* `srtla_rec.*` is, as a whole, governed by the AGPL-3.0: if you
  modify it and run it as a network service, you must publish your source.
- **Downstream (e.g. srt-live-server).** A binary that links this libsrt build
  *with* the SRTLA extension inherits the same AGPL obligation; building against
  a libsrt without `srtla_rec.*` does not.

## Attribution

SRTLA (SRT Link Aggregation) as a protocol originates with the BELABOX / OpenIRL
`srtla` project. This libsrt-integrated receiver is an independent implementation
of that protocol; it does not incorporate source code from the standalone
`srtla_rec` / `srtla_send` programs.
