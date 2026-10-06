# Notice

CyberCraft's design (a hidden Minecraft Fabric mod linked to the game through shared memory), the
shape of its protocol, and parts of its Gradle setup and Windows shared-memory code follow
[SkyCraft](https://github.com/chasmlol/SkyCraft), which is MIT-licensed:

> MIT License
>
> Copyright (c) 2026 chasmlol
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The RED4ext plugin is built on [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK) (MIT).

The way Minecraft's picture is split into a world layer (colour and depth, copied just before the hand is drawn) and an overlay layer
(hand, hotbar and screens, copied at the end of the frame), and the fix that restores the OpenGL read buffer after a depth readback
(`GlCommandEncoderMixin`), follow the "Minecraft x GTA V" example in
[universal-modder](https://github.com/rehan-remade/universal-modder), which is MIT-licensed:

> MIT License
>
> Copyright (c) 2026 Rehan and universal-modder contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
> EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
> IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
> OR OTHER DEALINGS IN THE SOFTWARE.

## Codeware (MIT, psiberx)
The collider script (cyberpunk/scripts/Colliders.reds) uses Codeware's callback, static entity and reflection scripting API, and follows its documentation on adding components while an entity is assembled. https://github.com/psiberx/cp2077-codeware
