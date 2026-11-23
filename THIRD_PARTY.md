# Third-party code

## Tracker replayers

`src/sw/ay_players.cpp` implements the PT3 / PT2 / STC pattern semantics used by this player.
It is a clean implementation of the public module formats and does not include a third-party
replayer library.

## YM2149 (tsconf)

`src/rtl/sound/ym2149.v` is the YM2149 model from the tsconf core
(`karabas-go-core-tsconf-master/src/rtl/sound/ym2149.v`), by MikeJ and Sorgelig. It is used
twice as the Turbo Sound pair, with `MODE=0` (the YM2149 volume table).

```
Copyright (c) MikeJ - Jan 2005
Copyright (c) 2016-2019 Sorgelig

All rights reserved

Redistribution and use in source and synthezised forms, with or without modification, are
permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer.
2. Redistributions in synthesized form must reproduce the above copyright notice, this list
   of conditions and the following disclaimer in the documentation and/or other materials
   provided with the distribution.
3. Neither the name of the author nor the names of other contributors may be used to endorse
   or promote products derived from this software without specific prior written permission.

THIS CODE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## make_core.py

`src/make_core.py` is a copy of the `make_core` tool from
[karabas-go-tools](https://github.com/andykarpov/karabas-go-tools), MIT License,
Copyright (c) 2024 Andy Karpov. It is vendored so the repository builds without a second
checkout.
