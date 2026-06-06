# 数据格式与打开策略

## 总原则

光谱程序必须把三类数据分开处理：

- 原始 FITS 光谱。
- 已经处理好的 `.npy` 光谱矩阵。
- 星表/catalog FITS。

星表 FITS 不是一条光谱，不能直接按 `flux-wavelength` 画图。当前已实现的真实数据读取能直接画的是：

- 两列波长/流量 `.csv` 光谱。
- 一维 `.npy` 光谱数组。
- 二维 `.npy` 光谱矩阵中的一行。
- 可识别的单条 FITS table 光谱：LAMOST/SDSS table 路径。

打开 FITS 时，如果文件里找不到可识别的单条光谱结构，应提示“这是 catalog 或不支持的 FITS，不是单条光谱”，而不是把星表列误当成光谱曲线。

当前 native loader 是同步 UI 路径，只面向单条光谱级别文件。为避免误开大型 catalog FITS 时卡 UI 或占用过多内存，FITS/FITS.GZ 在读取和解压前有大小上限；超过上限时应返回 domain error snapshot，而不是继续尝试整文件解析。

当前 FITS reader 是窄口径 vertical slice，不做通用 FITS。后续如果继续扩张 FITS 支持，应先把实现从通用 loader 文件拆到独立 `fits_spectrum_loader` 边界，并优先评估 CFITSIO/CCfits，而不是继续堆手写 FITS 语义。

## CSV 读取

当前 CSV loader 支持带表头的简单两列光谱，常见列名是：

```text
wav, flux
wavelength, flux
loglam, flux
```

如果使用 `loglam`，波长按 `10 ** loglam` 转换；如果使用 `wav`/`wavelength`，直接视为 Angstrom 波长。读取后清理不可解析、非有限、非正波长的行，并按波长升序绘图。CSV 不携带 rest-frame 校正状态，因此只能作为未确认坐标系的波长轴显示。

## 文件夹读取

Folder source 是一个由多个文件组成的光谱集合，只读取目录第一层，不递归进入子文件夹。当前纳入集合的文件类型是：

- `.csv`
- `.fits`
- `.fit`
- `.fts`
- `.fits.gz`

目录中的子文件夹不会递归加载，应写入 warning diagnostic。其它文件类型也应忽略并写入 warning diagnostic。CSV 和 FITS 混在同一个目录时仍可加载，但也应写入 warning diagnostic，因为这通常表示数据批次不纯，需要业务侧确认。集合内部按文件名稳定排序，UI 的上一条/下一条在这些文件之间切换。

## 统一波长网格

项目里很多处理后数据都对齐到固定 loglam 网格：

```text
loglam_i = 3.5682 + i * 0.0001
len = 3909
wavelength_i = 10 ** loglam_i
```

如果 `.npy` 的列数是 `3909`，默认按这个波长轴画。其他长度不要硬套这个网格，可以先退回像素序号，也可以要求文件提供波长信息。

## NPY 读取

`*_X.npy` 或 `*_flux.npy` 才是主要光谱矩阵。二维数组按行读取，每一行是一条光谱。

读取时优先使用：

```python
np.load(path, mmap_mode="r", allow_pickle=False)
```

这样大矩阵不会一次性读进内存。旁边如果有同前缀的 `*_name.npy`，且行数一致，就用它作为样本名。

这些辅助数组不要当作主光谱打开：

- `*_y.npy`
- `*_label.npy`
- `*_index.npy`
- `*_ormask.npy`
- `*_inverse.npy`
- `*_known_mask.npy`

`X.npy` 通常已经是样本级 z-score/SNV 后的模型输入，不是原始 flux。画 `X.npy` 时 y 轴应理解为 normalized flux / feature value，不要再自动归一化一次。若有 `flux.npy + ormask.npy`，后续可以提供“原始 flux”和“模型输入 X”两种视图。

## LAMOST DR10 / dr10_v1.0

旧主数据里前 7396 条是 LAMOST DR10 FITS，常见位置类似：

```text
data/Carbon_Spectral/carbon_dr10(int.)_11550_fits
data/new_carbon_candidates/Final_matched_fits(dr10_v1.0)
```

DR10 FITS 常见 image HDU 结构是：

- `data[0]` 是 flux。
- `data[1]` 是 inverse variance。
- `data[4]` 是 ormask。

波长不要猜，应使用 header：

```text
wavelength = 10 ** (COEFF0 + COEFF1 * pixel)
```

这里 `COEFF1` 通常应为 `0.0001`。原始光谱可能不覆盖完整 3909 网格，首尾无覆盖位置在 raw flux/mask 里可能是 `NaN`。

当前 native loader 仅在 header 明确提供 `COEFF0/COEFF1` 时把这种 image 结构作为受限 fallback 识别；这不是可靠 image FITS 或通用 FITS 支持承诺。不要用 `CRVAL1/CD1_1` 等 WCS 字段猜测 log10 wavelength，除非后续同时实现并验证 `CTYPE/DC-FLAG` 等语义。

FITS 读取需要把“观测波长轴”与“到目标静止系”分开。观测波长仍来自
`COEFF0/COEFF1`、`WAVELENGTH` 或 `LOGLAM`，不要再做一次 FITS/WCS 波长校准。
如果后续要显示目标静止系，核心关系是：

```text
lambda_rest = lambda_observed / (1 + z)
z ~= radial_velocity_km_s / 299792.458
```

这里的低速近似只适合恒星视向速度。`HELIO_RV` 是观测者运动/日心框架修正，
不是目标静止系速度，不能拿它再把谱线“修一次”。LAMOST/SDSS 光谱通常是真空
波长，谱线表也必须使用真空波长；例如 Hα 应用约 `6564.614 Å`，不是空气波长
`6562.801 Å`。

读取器应在不改变当前绘图波长的前提下，优先记录可验证来源，例如
`wavelength_medium`、`observer_frame_correction`、`radial_velocity_km_s`、
`redshift`、`radial_velocity_source`、`target_redshift`、`target_redshift_status`
和 `rest_frame_correction_status`。恒星优先使用官方 RV 并按低速近似推导
`target_redshift`；星系/QSO 使用 pipeline `Z`。无效值必须挡住，例如 LAMOST
常见 `Z=-9999`；`ZWARNING` 非零时应显示为不可靠/需人工确认。只读到 RV 或
redshift 不能清除 UI 警告。只有在明确完成观测波长到 rest-frame 的显示或数据
校正后，才应同时把 `x_axis_frame` 标为 `rest`，并把
`rest_frame_correction_status` 标为 `applied`、`verified` 或等价状态。`unknown`
frame 仍可作为参考显示 rest-frame 标准线表并显示黄色叹号；`observed` frame
默认不显示 rest-frame 标准线表，除非 `rest_frame_correction_status` 明确表示校正已应用。

## LAMOST DR13 LRS

DR13 是当前低分辨率大头之一，catalog 在：

```text
data/Carbon_Spectral/lamost_dr13_v1.0_LRS/dr13_v1.0_LRS_catalogue.fits.gz
```

单条光谱常见列是：

```text
WAVELENGTH, FLUX, ORMASK
```

有效像素规则是：

```text
finite(wavelength) & finite(flux) & wavelength > 0 & ORMASK == 0
```

也就是 `mask == 0` 才画作有效点。LAMOST 的 `class/subclass` 是 survey pipeline/catalog 标签，不要直接写成严格真值。

## SDSS DR17 / DR19

SDSS 光谱优先按 table HDU 读，常见核心列是：

```text
FLUX, LOGLAM, IVAR, AND_MASK, OR_MASK
```

如果有 `LOGLAM`，波长是 `10 ** LOGLAM`；如果有 `WAVELENGTH`，直接用。

有效像素优先看 `IVAR`：

```text
finite(loglam/wavelength) & finite(flux) & finite(ivar) & ivar > 0
```

`IVAR == 0` 的点应忽略。`AND_MASK/OR_MASK` 是位掩码，可作为显示质量信息，但项目构建里最核心的有效性判断是 `ivar > 0`。

DR17 的 catalog 是 `specObj-dr17.fits`，DR19 是 `spAll-v6_1_3.fits.gz`。DR17 的 `SUBCLASS` 含 `Carbon`、`CarbonWD`、`Carbon_lines` 被当作 carbon-like 候选；DR19 主要用 `SUBCLASS == Carbon`。这些都是 survey 自带分类，适合做候选池，不等于可靠真值。

SDSS 还有两个坑：

- repeat/coadd 可能全零或 `ivar` 全 0，这类不要当正常谱。
- DR19 有少数极端 flux outlier，可能到 `1e8` 量级。自动 y 轴最好提供 percentile/robust 缩放，否则正常谱线会被压扁。显示时可以缩放视图，但不要默认改写原始数据。

## FITS 读取优先级

打开 FITS 时建议按这个顺序：

1. 找 table HDU：必须有 `flux`，并且有 `loglam` 或 `wavelength`。
2. 找受限 image fallback：必须有 `COEFF0/COEFF1`；第 0 行是 flux；第 1 行存在时按 `ivar > 0` 过滤；第 4 行存在时按 `ormask == 0` 过滤；不使用 `CRVAL1/CD1_1` 猜测 log10 wavelength。
3. 读取可验证的目标 RV 或红移元数据，挡住无效 redshift，标记不可靠 redshift，并记录当前是否有可用的 `target_redshift`；不要使用 `HELIO_RV` 作为目标速度。
4. 清理非有限 wavelength/flux、非正 wavelength，并按 wavelength 升序画。
5. 如果找不到这些信息，提示“这是 catalog 或不支持的 FITS，不是单条光谱”。

## DR 选择口径

在当前净增数据集中，选谱优先级通常是：

```text
LAMOST DR13 -> SDSS DR17 -> SDSS DR19
```

但 V0.2 里新增的 SDSS DR17 自报碳星层，优先使用 DR17 自己那条 `spec-PLATE-MJD-FIBER.fits`，不要随便替换成同位置的非自报 repeat spectrum。

## 一句话总结

程序层面不要把所有文件都当成同一种结构。`NPY` 是行级矩阵，FITS 要先识别 HDU、波长列或受限 `COEFF0/COEFF1` fallback，`mask/ivar` 决定有效点，`X.npy` 是处理后的特征而不是原始流量。
