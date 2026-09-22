#!/usr/bin/env python3
"""Generate the SolarOS pinyin IME tables.

Produces src/services/solar_os_ime_table.h/.c:
  - syllable table (sorted by pinyin) -> candidate range
  - candidate data (UTF-8 hanzi / words)

Sources:
  - GB2312 level-1 hanzi (0xB0A1..0xD7F9, 3755 chars)
  - a built-in list of common two-character words
Readings come from pypinyin (all heteronyms).
"""
from __future__ import annotations

from pathlib import Path

from pypinyin import pinyin, Style

OUT_H = Path(__file__).resolve().parents[1] / "src" / "services" / "solar_os_ime_table.h"
OUT_C = Path(__file__).resolve().parents[1] / "src" / "services" / "solar_os_ime_table.c"

PYIN_MAX = 7
CAND_MAX = 12
CAND_PER_SYLLABLE = 24  # candidates stored per syllable (paging needs >8)

# Additional (less-common) readings for frequent polyphones.  pypinyin's
# heteronym=True data contains bogus entries (e.g. "家" -> jie), so the
# generator uses the primary reading plus this curated extra list only.
COMMON_DUOYIN = {
    "长": ["zhang"], "行": ["hang"], "重": ["chong"], "乐": ["yue"],
    "还": ["huan"], "数": ["shu"], "区": ["ou"], "种": ["zhong"],
    "得": ["dei"], "都": ["dou"], "会": ["kuai"], "觉": ["jiao"],
    "教": ["jiao"], "假": ["jia"], "调": ["tiao"], "度": ["duo"],
    "间": ["jian"], "单": ["chan", "shan"], "系": ["ji"], "切": ["qie"],
    "处": ["chu"], "石": ["dan"], "叶": ["she"], "色": ["shai"],
    "血": ["xie"], "藏": ["zang"], "宿": ["xiu"], "率": ["shuai"],
    "降": ["xiang"], "相": ["xiang"], "应": ["ying"], "兴": ["xing"],
    "露": ["lou"], "剥": ["bao"], "薄": ["bao"], "圈": ["juan"],
    "解": ["xie"], "地": ["de"], "给": ["ji"], "乐": ["yue"],
    "背": ["bei"], "难": ["nan"], "传": ["zhuan"], "曲": ["qu"],
    "卷": ["juan"], "卡": ["qia"], "削": ["xue"], "将": ["jiang"],
    "尽": ["jin"], "弄": ["nong"], "弹": ["tan"], "折": ["she"],
    "挑": ["tiao"], "据": ["ju"], "攒": ["cuan"], "晕": ["yun"],
    "劲": ["jing"], "咽": ["ye"], "呢": ["ni"], "哪": ["ne"],
    "什": ["shi"], "干": ["qian"], "大": ["dai"], "台": ["tai"],
    "说": ["shui"], "晃": ["huang"], "扇": ["shan"], "模": ["mu"],
    "糊": ["hu"], "夹": ["ga", "jia"], "累": ["lei"], "读": ["dou"],
    "识": ["zhi"], "扎": ["zha"], "轧": ["ya"], "量": ["liang"],
    "结": ["jie"], "服": ["fu"], "畜": ["xu"], "吓": ["he"],
    "爪": ["zhua"], "占": ["zhan"], "系": ["xi"], "待": ["dai"],
}

COMMON_WORDS = [
    "我们", "他们", "你们", "她们", "自己", "什么", "怎么", "这样", "那样", "这个",
    "那个", "这些", "那些", "时候", "现在", "以后", "之前", "今天", "明天", "昨天",
    "早上", "晚上", "中午", "中国", "北京", "上海", "深圳", "广州", "广东", "江苏",
    "浙江", "四川", "山东", "武汉", "成都", "西安", "南京", "天津", "重庆", "音乐",
    "歌曲", "播放", "停止", "暂停", "继续", "电台", "广播", "网络", "收音机", "天气",
    "新闻", "手机", "电脑", "蓝牙", "键盘", "触摸", "屏幕", "文件", "文件夹", "打开",
    "关闭", "保存", "删除", "新建", "编辑", "复制", "剪切", "粘贴", "搜索", "下载",
    "上传", "连接", "断开", "设置", "帮助", "信息", "工作", "学习", "休息", "谢谢",
    "再见", "你好", "请问", "应该", "可以", "知道", "名字", "时间", "房间", "问题",
    "回答", "开始", "结束", "成功", "失败", "错误", "正确", "速度", "温度", "电压",
    "电流", "电源", "充电", "电池", "信号", "声音", "视频", "图片", "照片", "文字",
    "邮件", "电话", "短信", "地址", "密码", "账号", "用户", "登录", "注册", "退出",
    "重启", "关机", "开机", "显示", "地图", "路线", "导航", "天气", "预报", "资讯",
    "直播", "回放", "录音", "播放器", "设备", "主板", "芯片", "固件", "系统", "版本",
    "更新", "升级", "恢复", "备份", "数据", "存储", "内存", "磁盘", "分区", "目录",
    "路径", "命令", "终端", "日志", "调试", "测试", "开发", "编程", "程序", "软件",
    "硬件", "免费", "收听", "频道", "选择", "输入", "确认", "取消", "确定", "返回",
]


def norm(ph: str) -> str:
    out = []
    for c in ph.lower():
        if "a" <= c <= "z":
            out.append(c)
        elif c == " ":
            out.append(" ")
    s = "".join(out).strip()
    return (s.replace("ü", "v").replace("u:", "v")
             .replace("û", "v").replace("ū", "u"))


def readings(ch: str) -> list[str]:
    """Primary reading (pypinyin default) plus curated polyphone extras.

    pypinyin's heteronym=True data is unreliable (e.g. 家 -> jie), so we
    never trust it wholesale; the default reading is the common one and
    COMMON_DUOYIN adds the frequent secondary readings we care about.
    """
    res = []
    try:
        py = pinyin(ch, style=Style.NORMAL, heteronym=False)
    except Exception:
        return []
    if py and py[0]:
        s = norm(py[0][0])
        if s:
            res.append(s)
    for extra in COMMON_DUOYIN.get(ch, []):
        e = norm(extra)
        if e and e not in res:
            res.append(e)
    return res


def gb2312_l1() -> list[str]:
    chars = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFB):
            try:
                s = bytes([hi, lo]).decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                chars.append(s)
    return chars


def main() -> int:
    syll: dict[str, list[str]] = {}

    for ch in gb2312_l1():
        for ph in readings(ch):
            syll.setdefault(ph, []).append(ch)

    for w in dict.fromkeys(COMMON_WORDS):
        parts = []
        ok = True
        for c in w:
            rs = readings(c)
            if not rs:
                ok = False
                break
            parts.append(sorted(rs))
        if not ok:
            continue
        combos = [""]
        for rs in parts:
            combos = [a + b for a in combos for b in rs]
            if len(combos) > 64:
                break
        for ph in combos:
            if 0 < len(ph) <= PYIN_MAX - 1:
                syll.setdefault(ph, []).append(w)

    syllables = sorted(syll.keys())

    cand_data = bytearray()
    cand_index: dict[str, tuple[int, int]] = {}
    for ph in syllables:
        seen: list[str] = []
        for c in syll[ph]:
            if c not in seen:
                seen.append(c)
        block = seen[:CAND_PER_SYLLABLE]
        off = len(cand_data)
        for c in block:
            b = c.encode("utf-8")
            if 0 < len(b) < CAND_MAX:
                cand_data.append(len(b))
                cand_data += b
        cand_index[ph] = (off, len(block))

    lines_h = [
        "/* Auto-generated by fonts/gen_ime_table.py. Do not edit. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    char pinyin[8];",
        "    uint16_t cand_off;",
        "    uint16_t cand_count;",
        "} solar_os_ime_syllable_t;",
        "",
        "extern const solar_os_ime_syllable_t solar_os_ime_syllables[];",
        "extern const uint32_t solar_os_ime_syllable_count;",
        "extern const uint8_t solar_os_ime_candidates[];",
        "extern const uint32_t solar_os_ime_candidate_size;",
        "",
    ]
    OUT_H.write_text("\n".join(lines_h), encoding="ascii")

    lines_c = [
        "/* Auto-generated by fonts/gen_ime_table.py. Do not edit. */",
        "",
        '#include "solar_os_ime_table.h"',
        "",
        f"const uint32_t solar_os_ime_syllable_count = {len(syllables)};",
        f"const uint32_t solar_os_ime_candidate_size = {len(cand_data)};",
        "",
        f"const solar_os_ime_syllable_t solar_os_ime_syllables[{len(syllables)}] = {{",
    ]
    for ph in syllables:
        off, cnt = cand_index[ph]
        lines_c.append(f'    {{ "{ph}", {off}, {cnt} }},')
    lines_c.append("};")
    lines_c.append("")
    lines_c.append("const uint8_t solar_os_ime_candidates[] = {")
    for i in range(0, len(cand_data), 16):
        chunk = cand_data[i:i + 16]
        lines_c.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines_c.append("};")
    lines_c.append("")
    OUT_C.write_text("\n".join(lines_c), encoding="ascii")

    total = sum(v[1] for v in cand_index.values())
    print(f"wrote {OUT_H} + {OUT_C}")
    print(f"syllables={len(syllables)} candidates={total} "
          f"cand_data={len(cand_data)}B table={len(syllables)*12}B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())