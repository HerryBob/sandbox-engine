# 走查：调色板存储 + 方向 A（A1 / A2 / A3）

本文按「文件 → 函数 → 改了什么 → 为什么 → 怎么验」逐条列，顺序就是代码改动的实际顺序，可以一边看本文一边打开对应文件对。

行号对应本次提交前的工作区状态；若行号漂移，按函数名定位。

---

## 零、走查前置

### 0.1 构建

```
$env:VSLANG = "1033"
$msvc = "E:\VS2026\VC\Tools\MSVC\14.51.36231"
$kits = "E:\Windows Kits\10"
$env:INCLUDE = "$msvc\include;$kits\Include\10.0.26100.0\ucrt;$kits\Include\10.0.26100.0\um;$kits\Include\10.0.26100.0\shared"
$env:LIB     = "$msvc\lib\x64;$kits\Lib\10.0.26100.0\um\x64;$kits\Lib\10.0.26100.0\ucrt\x64"
$env:PATH    = "$msvc\bin\Hostx64\x64;$env:PATH"
& "E:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "E:\MinecraftGame2\build\Debug"
```

三条必须先设：`INCLUDE`、`LIB`、`PATH`。少一条就是 `LNK1104 kernel32.lib`。
改过 `.hpp` 之后必须加 `--clean-first`——本机 `VSLANG=1033` 不起作用，cl 的中文 `/showIncludes` 会让 Ninja 的依赖记录失效，增量构建会漏重编。

### 0.2 跑测

```
Start-Process "E:\MinecraftGame2\build\Debug\sandbox_engine.exe" `
  -WorkingDirectory "E:\MinecraftGame2\build\Debug" `
  -RedirectStandardOutput "...\run_default.txt" -RedirectStandardError "...\run_default_err.txt" -PassThru
```

- default 预设等 4 秒，noise 等 8 秒，然后 `CloseMainWindow()`，再轮询 `HasExited` 最多 20 秒。
- noise 参数：`-ArgumentList "--preset","noise"`。
- 注意 PowerShell 里不要把函数参数命名成 `$args`——那是保留变量，会把参数吞掉，导致 noise 实际跑成 default（本轮踩过一次）。

### 0.3 判定基线

| 预设 | 方块 | 地表 y | chunk | 调色板 | 字典均值 | 占用 | 省 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| default | 82944 | 3..3 | 81 | 4 位 | 4.00 | 163 KB | 74.80% |
| noise | 695641 | 26..38 | 220 | 4 位 | 2.77 | 442 KB | 74.86% |

两个预设 `EXIT=0`、stderr 长度 0，就是通过。

---

## 一、调色板（palette，区块方块存储的局部字典）

### 1.1 `src/shared/block/Block.hpp`

这是改造的核心文件。改造前 `ChunkData` 是 `std::array<std::uint16_t, 4096>`，每区块固定 8192 字节，不管里面装了几个方块种类。

| 行 | 内容 | 改了什么 |
| --- | --- | --- |
| 19 | `using PaletteEntry = std::uint32_t;` | 方块条目类型。用 32 位是为了将来装 state（状态），现在只装方块 id |
| 22 | `kAirEntry` | 空气常量，初值字典里只有它 |
| 73 | `class ChunkData` | 从 `struct` 改成 `class`。原因：现在有行为（扩宽、查字典、判空），不再只是裸数据 |
| 78 / 80 | `get(x,y,z)` / `set(x,y,z,e)` | **接口替换**。原 `atWorld` 直接返回 `uint16` 引用，现在读写都经过字典，不保留旧名 |
| 84 | `getAt(i)` | 按线性下标读，给生成器和遍历用 |
| 87 | `isAllAir()` | 空区块判定，决定要不要建 mesh |
| 93 / 94 | `bits()` / `paletteSize()` | 给统计和自检用 |
| 96 | `dirty` | 保持原样，变更原因位掩码，与调色板无关 |
| 113 / 114 | `indexAt` / `setIndexAt` | 私有。索引 → 字典下标的位打包读写 |
| 116 | `findOrAdd(e)` | 私有。线性扫字典，没有就追加 |
| 118 | `growTo(newBits)` | 私有。4 → 8 → 16 扩宽 |
| 120 | `palette_` | 局部字典，初值只有 Air |
| 123 | `bits_ = 4` | 起始位宽 4（最多 16 种方块），够覆盖当前 6 种 |

**为什么只做 4 / 8 / 16 三档**：不做 12 位，不做跨 word 位打包。三档正好对应「≤16 种」「≤256 种」「≤65536 种」，越档就整块换宽度，实现简单、没有位跨界读写。跨 word 打包要处理索引跨字节边界的位移，收益只有 4 位 vs 12 位的中间档，不值得。

**关键实现点（按顺序对）**：

- `indexAt`（131）：按 `bits_` 分三支。4 位从 `packed_` 里取一个字节，偶数下标取低 4 位、奇数取高 4 位。8 位直接取 `packed_[i]`。16 位取 `wide_[i]`。
- `setIndexAt`（140）：与 `indexAt` 严格互逆。4 位时先清掉目标半字节再写入，不能直接或——否则旧值残留。
- `findOrAdd`（152）：线性扫。命中返回下标；没命中 `push_back`，然后检查 `palette_.size() > (1 << bits_)`，越了就 `growTo`。
- `growTo`（192）：先 `indexAt` 把全部 4096 个下标读进临时 `std::vector<std::uint16_t>`，再切 `bits_`，新建一块按新宽度分配的存储，写回，`swap`。**不能边读边写**——源和目标宽度不同，就地转换会读到自己刚写的字节。
- `get`（163）：`palette_[indexAt(indexOf(x,y,z))]`。`indexOf` 是 `(z&15)*256 + (y&15)*16 + (x&15)`。
- `set`（167）：`setIndexAt(i, findOrAdd(e))`。
- `isAllAir`（171）：字典只剩 1 项时一次比较就返回；否则逐格扫。这是调色板带来的第一个实际收益——空区块判定从 4096 次比较降到 1 次。

**怎么验**：跑起来看 `[Palette] chunk=81 4位=81 8位=0 16位=0 平均字典=4.00项 实际=163KB 密集数组本应=648KB（省 74.80%）`。四个数对得上就说明打包、字典、统计三处一致。

### 1.2 `src/business/World.cpp` / `World.hpp`

| 位置 | 改了什么 |
| --- | --- |
| `World.hpp:57` | 新增 `PaletteStats paletteStats() const;` 声明 |
| `World.cpp` 全文 | 6 处原 `atWorld(...)` 调用点改成 `get(...)` / `set(...)` |
| `World.cpp:130` | `paletteStats()` 实现：遍历所有 chunk，统计位宽分布、字典总项数、`byteSize()` 合计、与密集数组对比算节省比例 |

注意 `World` 自己的 `BlockState` 枚举（`NotLoaded` / `Air` / `Occupied`）和调色板**没有关系**，只是名字撞车，在另一个类里，别混。

**为什么扩宽不标 dirty**：位宽变化不改变任何方块的对外取值，`get` 出来的结果完全一样。标 dirty 会触发一轮没有实际变化的 mesh 重建，纯浪费。

### 1.3 `src/business/base/BlockActions.cpp`

| 行 | 函数 | 改了什么 |
| --- | --- | --- |
| 19 | `tryPlace` | 写方块改走 `ChunkData::set` |
| 36 | `tryRemove` | 删方块改走 `ChunkData::set(..., kAirEntry)` |

`slot = data` 这种整体覆盖的写法仍然可用，赋值运算符没动。

### 1.4 `src/worldgen/generator/ColumnGenerator.cpp`

`L138`：`out.chunks[y >> 4].set(worldX, y, worldZ, static_cast<PaletteEntry>(type))`。

这一行是唯一一处生成器直接写方块的地方。`y >> 4` 是**层号**（4 个 16 高的子块拼成一列 64 高），写入时用世界坐标，`indexOf` 内部 `&15` 折到块内坐标。

### 1.5 `src/main.cpp` 自检 `selfTestPalette()`（L76）

四条断言：默认 4 位、越 16 种自动扩到 8 位、扩宽后旧值不丢、判空正确。

**这里有个必须知道的坑**：第一版写的是 `a.set(i, 0, 0, ...)`，`i` 从 16 到 19。但 `indexOf` 会 `x & 15`，`i=16` 折回 0，正好覆盖前四格，自检报失败。改成 `x = i % 16`、`y = (i / 16) % 16` 之后通过——**是测试代码写错，`ChunkData` 本身没问题**。走查时看到这段循环要多看一眼。

`main.cpp:251` 调用，`main.cpp:399` 打印 `[Palette]` 统计。

---

## 二、A1：模型 parent 父子继承

目标：让 `wood.json` 这种「六个面贴图不同、几何完全一样」的模型，不用把 6 个面重抄一遍。

### 2.1 `src/render/assets/ModelDef.hpp`

`L53` 新增 `std::string parent;`，注释写明是父模型名（如 `block/cube_all`），空表示没有父。

### 2.2 `src/resource/DataRegistry.hpp`

`L68` 新增 `forEachValue(fn)`。原来只能按名字 `find`，没法遍历全部已加载模型——而父模型解析必须遍历，因为子模型可能比父模型先加载。

### 2.3 `src/render/assets/ModelLoader.cpp`

| 行 | 函数 | 改了什么 |
| --- | --- | --- |
| 42 | `decodeModelDef` | 允许没有 `elements`：有 `parent` 就 `return true`，不再当解析失败 |
| 117 | `parentKey` | 把 `"block/cube_all"` 规整成注册表里的 key |
| — | `fallbackCube` | `parent` 指不到时回退的立方体 |
| — | `expandFaceTextures` | 把 `#all` 这类纹理变量展开成实际贴图名 |
| 149 | `resolveOne` | 单条解析：递归取父、合并、记忆化、环检测 |
| 201 | `resolveModelParents` | 遍历全部模型逐条解析；`L251` 在加载末尾调用 |

**烘焙语义（照 MC 不自创）**：

- `elements` **整段替换，不合并**。子模型有 `elements` 就用子模型的，父模型的整个丢掉。
- `textures` **按 key 合并，子覆盖父**。父给 `#all`，子只写 `#side`，最后两个都在。
- 循环引用判失败（`visiting` 集合，进栈标记、出栈清除）。
- `parent` 不存在：报错并用 `fallbackCube()` 兜底，不让整条链崩掉。
- **纹理变量展开的时机从解码期挪到烘焙后**。解码期父还没解析，`#all` 无从展开。

### 2.4 资源文件

- 新建 `assets\models\block\{cube_all,cube_column,cube_bottom_top}.json`——三个通用父模型。
- `mods_src\blocks\assets\models\block\wood.json` / `grass.json` 改成 `parent` 形式，只写差异部分。

### 2.5 怎么验

看日志 `[Model] parent 烘焙 5 条，失败兜底 0 条`，再往下看 5 条逐条诊断，父子关系和纹理展开结果要和 JSON 里写的一致。

---

## 三、A2：元素级 rotation（旋转）

目标：让单个元素能绕轴转，从而做出柱子、斜置板、楔形这类立方体拼不出来的形状。

### 3.1 `src/render/assets/ModelDef.hpp`

`L23` 新增 `struct ModelRotation { glm::vec3 origin{8,8,8}; char axis='y'; float angle=0; bool rescale=false; }`，`ModelElement` 增加 `rotation` 字段。

`L31` `applyModelRotation(p, r)`：把点平移到 origin 为原点，绕 `axis` 转 `angle` 度，再平移回去。`rescale` 是 MC 的约定——45° 时元素要按包围盒放大，否则旋转后边角会缩进格内。

### 3.2 `src/render/assets/ModelLoader.cpp`

`decodeModelDef` 里解析 `rotation` 对象：`origin`（默认 8,8,8）、`axis`（默认 y）、`angle`（默认 0）、`rescale`（默认 false）。

### 3.3 `src/render/VulkanMesh.cpp`

`L55` `buildModelVertices` 重写。三件事：

- `L81` / `L121`：每个角点算完位置后过一遍 `applyModelRotation`。
- 旋转后重新算包围盒中心和 `rescale` 放大。
- **面剔除仍按未旋转的 `cullFace` 判**。原因：剔除看的是「这个面朝不朝外」，旋转不改变元素在方块格内的朝外关系；按旋转后的面朝向判会把该保留的面剔掉。

### 3.4 `src/main.cpp`

- `L116` `selfTestModelRotation()`：4 条断言，绕 Z 90°、绕 Y 90°、绕 X 90°、origin 自身不动。
- `L158` `selfTestModelGeometry()`：整格元素绕 Z 45° 后断言 24 顶点、三轴跨度 1.414 / 1.414 / 1.0（容差 0.05）；把 `angle` 置 0 再断言三轴都是 1.0（容差 0.01）。

**编译坑**：`Vertex` 在 `engine::render` 命名空间，`main.cpp` 里直接写 `std::vector<Vertex>` 会报 `C2065 / C2923 / C2976`，必须加 `using engine::render::Vertex;`。

**怎么验**：两条自检都打印「通过」，然后两预设关键行与基线一致。

---

## 四、A3：图集声明式

目标：图集有哪几张图、多大，从代码常量改成 `assets/atlases/blocks.json` 的声明，边长按实际条目数算。

### 4.1 `src/render/assets/TextureAtlas.hpp`

| 改了什么 |
| --- |
| 删掉 `kAtlasGrid` / `kAtlasSize` 两个编译期常量 |
| 只留 `kAtlasTile = 16`（每格边长）、`kAtlasChannels = 4` |
| 新增 `build()`（原构造期建图集）、`grid()`、`size()` |
| `slotUV()` / `fallbackUV()` 从 static 改成实例 const 成员——UV 要用实例的 `grid_` 算 |
| 私有 `grid_ = 1`、`size_ = kAtlasTile` |
| `L33` `slotOf(name, out)` 按名字查格；`L46` `fallbackUV()` 恒为第 0 格 |
| `AtlasSlot{uint8 row, col}`，最大 255 格/边 |

### 4.2 `src/render/assets/TextureAtlas.cpp`

`L18` `build()` 七步：

1. 读 `atlases/blocks.json` 的 `textures` 数组（`g_resourceIndex.find(Assets, ...)` + rapidjson），逐项 `IsString`，跳过空名和 `default`，`std::set` 去重。
2. 扫方块注册表 + 模型注册表里的纹理引用，得到 `used` 集合。
3. 声明为空就回退用 `used`。
4. `need = 1 + declared.size()`（第 0 格永远留给 `default`），`grid_` 从 1 往上找第一个平方够大的，`size_ = kAtlasTile * grid_`，`pixels_.assign(size²*4, 0)`。
5. 第 0 格铺 `default.png`。
6. 按声明顺序从第 1 格开始铺：`slotIndex / grid_` 是行，`slotIndex % grid_` 是列。
7. 汇总「注册表引用了但没声明」的纹理，打 warn。

`L131` 打日志：`图集 48²（3×3 格）：声明 8 张，成功 8 张，失败 0 张，占用 9/9 格`。

**`L139` `addFromFile` —— 这里有一个今天才修的真 bug，走查必看：**

```cpp
// 现在的写法（正确）
const std::size_t pixelRow = static_cast<std::size_t>(slot.row) * kAtlasTile;
const std::size_t pixelCol = static_cast<std::size_t>(slot.col) * kAtlasTile;
std::uint8_t* base = pixels_.data() + (pixelRow * size_ + pixelCol) * kAtlasChannels;
for (std::uint32_t y = 0; y < kAtlasTile; ++y)
    std::memcpy(base + y * size_ * kAtlasChannels,
                src + y * kAtlasTile * kAtlasChannels,
                kAtlasTile * kAtlasChannels);
```

原来的写法是 `(slot.row * size_ + slot.col * kAtlasTile) * kAtlasChannels`——把**格行号**当**像素行号**用。正确应该是「格行 × 16 像素 × 图集宽」。详见第六节。

`L189` `slotUV(s)`：返回 `{col/grid, row/grid, (col+1)/grid, (row+1)/grid}`。

### 4.3 `src/render/VulkanContext.cpp`

- `L718-722` 原先的 static 调用改成实例调用（`textureAtlas_.fallbackUV()` / `slotUV()`）。
- `L825` `createTextureImage()`：先 `textureAtlas_.build()`，拿 `pixels`，`createImage(..., VK_FORMAT_R8G8B8A8_SRGB, ...)`，staging 缓冲 `vkMapMemory` + `memcpy`，`transitionImageLayout(UNDEFINED → TRANSFER_DST)`，`copyBufferToImage(staging, textureImage_, w, h)`，再转到 `SHADER_READ_ONLY`。`w/h/imageSize` 全部取自 `textureAtlas_.size()`。

### 4.4 `src/render/VulkanMesh.cpp`

`buildModelVertices` 里的 UV 取值改成实例调用：`atlas.fallbackUV()`、`atlas.slotOf(...)` + `atlas.slotUV(...)`。

### 4.5 `assets/atlases/blocks.json`

```json
{ "textures": ["stone", "log_side", "log_top", "dirt", "brick", "glass", "grass_top", "grass_side"] }
```

UTF-8 无 BOM，2 空格缩进。`default` 不用写——第 0 格自动占。

CMake 的 `assets` 是 ALL 目标 `copy_directory`，加子目录不用重新 configure。

### 4.6 怎么验两道门槛

- 8 项 → `图集 48²（3×3 格）…占用 9/9 格`
- 临时加一项到 9 项 → `图集 64²（4×4 格）…占用 10/16 格`，`EXIT=0`，82944 不变
- 验完把 `blocks.json` 写回 8 项

---

## 五、今天的故障：48² 下整屏只剩清屏色

这一节是走查时最该看的，因为它暴露的是**老代码里就藏着的 bug**，只是固定 128² 时恰好没被打到。

**现象**：8 项声明 → 48² 图集，画面整屏 RGB(39,39,63)，正好是 `clears[0].color = {{0.02,0.02,0.05,1.0}}` 经 sRGB 换算的结果。渲染通道执行了、清屏了，但没有任何片元落地，HUD 文字也没有。同一份代码把声明临时扩到 57 项强制回 128²，画面完全正常。

**排查路径**（记下来，下次同类问题可以照走）：

1. 先确认几何有没有生成：日志里 81 个 chunk 全部重建成功，`面=512/576/640`、`顶点=2048/2304/2560` 正常。
2. 排除尺寸相关：`createTextureImage` 里的 `w/h/imageSize` 全部取自 `size()`；`copyBufferToImage` 的 `bufferRowLength=0` 是紧凑布局，与图集内存一致；两次都无 resize / swapchain 事件。
3. **分叉实验**：临时把 `block.frag` 改成 `outColor = vec4(1,0,0,1)`、不 discard，48² 下整屏红色地形 → 证明几何一直在画，问题在片元被丢弃。
4. 定根因：`block.frag` 对所有方块执行 `if (c.a < 0.5) discard;`。采到的 alpha 是 0 才会全丢，说明 UV 指向的区域是空的。

**根因**：`addFromFile` 的像素偏移公式把「格行号」当「像素行号」。

```
错的：(slot.row * size_ + slot.col * kAtlasTile) * kAtlasChannels
对的：((slot.row * kAtlasTile) * size_ + slot.col * kAtlasTile) * kAtlasChannels
```

**为什么 128² 看起来正常**：`slot.row == 0` 时两式恰好相等。128² / 8×8 下前 7 张贴图全在 row 0，都对；只有 `grass_side`（row 1）错位，落到了别的有像素的区域，所以画得出来但纹理位置是错的。48² / 3×3 下 `grass_top`（slot 7）、`dirt`（slot 4）分别落在 row 2 / row 1，整块写到错误位置，草地上表面采到透明格子 → 被 `discard` 全丢 → 整屏只剩清屏色。默认预设地表全是草，所以是**整屏**而不是局部。

**修复后的验证**：48² 重跑，画面草地铺满 + 深蓝天空，采样像素 R52/G71/B34（y=0.25）、R41/G66/B24（y=0.5）、R38/G64/B22（y=0.85），`EXIT=0`、stderr 长度 0。

**截图工具注意**：不要用 `wscript.shell.AppActivate` + 全屏 `CopyFromScreen`——游戏窗口会被别的东西盖住，截到的是别人的画面。要 `GetWindowRect` + `SetWindowPos(HWND_TOPMOST)` + 按窗口矩形 `CopyFromScreen`，再用 GDI+ `Bitmap.GetPixel` 判色。

---

## 六、已知缺口（走查时不必当 bug 处理）

1. **边界壳面数没有改造前后逐帧对照**。调色板只影响存储，不影响面剔除，只有三条等价性推论；没有实测数字对照。
2. **`findOrAdd` 没有 `lastIndex_` 缓存**。当前每区块字典只有 2~4 项，线性扫代价可以忽略；等字典变大再说。
3. **`createTextureImage` 里 `vkUnmapMemory` 前没有 `vkBindMemory`**，属于双重 unmap。两种尺寸下都存在，与本次故障无关，未修。
4. **全部改动未提交**。调色板、A1、A2、A3 都还是工作区状态。

---

## 七、走查顺序建议

按代码依赖从底往上走，每一步都能独立看懂：

1. `Block.hpp`（字典与位打包，独立自洽）
2. `World.cpp` / `World.hpp`（调用点替换 + 统计）
3. `BlockActions.cpp`（放置 / 破坏）
4. `ColumnGenerator.cpp:138`（生成器唯一写入口）
5. `main.cpp` 的 `selfTestPalette()` 与 `[Palette]` 打印
6. `ModelDef.hpp` → `DataRegistry.hpp` → `ModelLoader.cpp`（A1 链）
7. `ModelDef.hpp` 的 `ModelRotation` → `VulkanMesh.cpp` 的 `buildModelVertices`（A2 链）
8. `main.cpp` 两条模型自检
9. `TextureAtlas.hpp` → `TextureAtlas.cpp`（A3，重点看 `addFromFile`）
10. `VulkanContext.cpp:825` → `VulkanMesh.cpp` 的 UV 取值（A3 接线）
11. `assets/atlases/blocks.json` 与两道尺寸门槛
