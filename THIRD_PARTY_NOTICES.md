# Third-Party Notices

本项目依赖以下第三方库（均为宽松许可证，与 MIT 兼容）。各库的完整许可证文本见其官方仓库链接。

| 库 | 版本/形态 | 许可证 | 版权声明 | 官方仓库 |
|---|---|---|---|---|
| **GLM**（OpenGL Mathematics） | header-only（third_party/include/glm） | MIT | Copyright (c) 2005 - G-Truc Creation | https://github.com/g-truc/glm |
| **GLFW** | 头文件 + glfw3.dll + glfw3dll.lib（third_party） | zlib | Copyright (c) 2002-2006 Marcus Geelnard, Copyright (c) 2006-2019 Camilla Löwy | https://github.com/glfw/glfw |
| **stb_image** | stb_image.h（third_party/include） | Public Domain 或 MIT（二选一） | Copyright (c) 2017 Sean Barrett | https://github.com/nothings/stb |
| **RapidJSON** | header-only（third_party/include/rapidjson） | MIT | Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip | https://github.com/Tencent/rapidjson |
| **Vulkan SDK**（运行时） | 本机 LunarG SDK（VULKAN_SDK 环境变量），不入库 | Apache-2.0（规范/头文件）、各组件各自许可 | — | https://vulkan.lunarg.com |

## 许可证全文

### GLM — MIT License

```
MIT License

Copyright (c) 2005 - G-Truc Creation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### GLFW — zlib License

```
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.
```

### stb_image — Public Domain / MIT

stb 系列库由 Sean Barrett 发布，**双许可**：可视为 Public Domain（CC0 等效），或选用 MIT License（文本同 GLM 上方 MIT 全文）。使用者任选其一。
