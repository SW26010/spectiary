# 测试与验证

按需要证明的行为选择流程；不同层级的通过结果不能互相替代。

| 要验证什么 | 入口 |
| --- | --- |
| CMake/CTest 的构建、fast / extended tier 与运行约定 | [工程环境](../development/engineering_setup.md) |
| Headless ImGui 控件的真实输入与生产 owner 结果 | [Widget regression tests](widget_testing.md) |
| 真实数据交互延迟、pan、导航、加载和资源稳定性 | [性能测试](performance_testing.md) |
| 真实进程业务自动化和 CI | [自动化导航](../development/automation/README.md) |
| Win32 / DXGI / Composition 事件、窗口缩放与显示证据 | [呈现诊断](../presentation/README.md) |

[架构决策](../adr/)和专题合同定义必须保持的行为；测试流程说明如何验证，日期[证据](../evidence/)保留一次运行或调查的结果与限制。测试命令均从仓库根目录执行。
