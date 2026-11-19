#!/usr/bin/env python3
"""Generate the firmware catalog from MiniMax's checked-in official reference."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE_URL = "https://github.com/MiniMax-AI/skills/blob/main/skills/frontend-dev/references/minimax-voice-catalog.md"
# A voice language is independent of the language codes accepted by V2V.
# In particular, yue belongs in the catalog but is not enabled in the V2V list.
GROUPS = {
    "Chinese Mandarin": ("zh", "male-qn-qingse"),
    "Chinese Cantonese": ("yue", "Cantonese_ProfessionalHost（F)"),
    "English": ("en", "English_Graceful_Lady"),
    "Japanese": ("ja", "Japanese_GracefulMaiden"),
    "Korean": ("ko", "Korean_CalmLady"),
    "Spanish": ("es", "Spanish_SereneWoman"),
    "Portuguese": ("pt", "Portuguese_SereneWoman"),
    "French": ("fr", "French_Male_Speech_New"),
    "Indonesian": ("id", "Indonesian_CalmWoman"),
    "German": ("de", "German_FriendlyMan"),
    "Russian": ("ru", "Russian_ReliableMan"),
    "Italian": ("it", "Italian_Narrator"),
    "Arabic": ("ar", "Arabic_CalmWoman"),
    "Turkish": ("tr", "Turkish_CalmWoman"),
    "Ukrainian": ("uk", "Ukrainian_CalmWoman"),
    "Dutch": ("nl", "Dutch_kindhearted_girl"),
    "Vietnamese": ("vi", "Vietnamese_kindhearted_girl"),
    "Thai": ("th", "Thai_female_1_sample1"),
    "Polish": ("pl", "Polish_female_1_sample1"),
    "Romanian": ("ro", "Romanian_female_2_sample1"),
    "Greek": ("el", "Greek_female_1_sample1"),
    "Czech": ("cs", "czech_male_1_v1"),
    "Finnish": ("fi", "finnish_male_3_v1"),
    "Hindi": ("hi", "hindi_male_1_v2"),
}

# Official Chinese names are translated for the English-only UI; raw IDs stay exact.
ENGLISH_NAMES = {
    "male-qn-qingse": "Youthful Man",
    "male-qn-badao": "Domineering Young Man",
    "male-qn-daxuesheng": "University Student",
    "female-shaonv": "Young Maiden",
    "female-yujie": "Elegant Woman",
    "female-chengshu": "Mature Woman",
    "female-tianmei": "Sweet Woman",
    "clever_boy": "Clever Boy",
    "cute_boy": "Cute Boy",
    "lovely_girl": "Lovely Girl",
    "cartoon_pig": "Cartoon Pig Xiao Qi",
    "bingjiao_didi": "Tsundere Younger Brother",
    "junlang_nanyou": "Handsome Boyfriend",
    "chunzhen_xuedi": "Innocent Junior Student",
    "lengdan_xiongzhang": "Aloof Senior Student",
    "badao_shaoye": "Domineering Young Master",
    "tianxin_xiaoling": "Sweetheart Xiao Ling",
    "qiaopi_mengmei": "Playful Cute Girl",
    "wumei_yujie": "Charming Mature Woman",
    "diadia_xuemei": "Flirty Junior Girl",
    "danya_xuejie": "Elegant Senior Girl",
    "Arrogant_Miss": "Arrogant Miss",
    "Robot_Armor": "Robotic Armor",
    "Chinese (Mandarin)_Reliable_Executive": "Reliable Executive",
    "Chinese (Mandarin)_News_Anchor": "News Anchor",
    "Chinese (Mandarin)_Mature_Woman": "Tsundere Mature Woman",
    "Chinese (Mandarin)_Unrestrained_Young_Man": "Unrestrained Young Man",
    "male-qn-jingying": "Elite Young Man",
    "Chinese (Mandarin)_Kind-hearted_Antie": "Kind-hearted Auntie",
    "Chinese (Mandarin)_HK_Flight_Attendant": "HK Flight Attendant",
    "Chinese (Mandarin)_Humorous_Elder": "Humorous Elder",
    "Chinese (Mandarin)_Gentleman": "Gentleman",
    "Chinese (Mandarin)_Warm_Bestie": "Warm Bestie",
    "Chinese (Mandarin)_Male_Announcer": "Male Announcer",
    "Chinese (Mandarin)_Sweet_Lady": "Sweet Lady",
    "Chinese (Mandarin)_Southern_Young_Man": "Southern Young Man",
    "Chinese (Mandarin)_Wise_Women": "Wise Woman",
    "Chinese (Mandarin)_Gentle_Youth": "Gentle Youth",
    "Chinese (Mandarin)_Warm_Girl": "Warm Girl",
    "Chinese (Mandarin)_Kind-hearted_Elder": "Kind-hearted Grandmother",
    "Chinese (Mandarin)_Cute_Spirit": "Cute Spirit",
    "Chinese (Mandarin)_Radio_Host": "Radio Host",
    "Chinese (Mandarin)_Lyrical_Voice": "Lyrical Voice",
    "Chinese (Mandarin)_Straightforward_Boy": "Straightforward Boy",
    "Chinese (Mandarin)_Sincere_Adult": "Sincere Young Adult",
    "Chinese (Mandarin)_Gentle_Senior": "Gentle Senior Girl",
    "Chinese (Mandarin)_Stubborn_Friend": "Stubborn Childhood Friend",
    "Chinese (Mandarin)_Crisp_Girl": "Crisp Girl",
    "Chinese (Mandarin)_Pure-hearted_Boy": "Pure-hearted Neighbor Boy",
    "Chinese (Mandarin)_Soft_Girl": "Soft Girl",
    "Cantonese_ProfessionalHost（F)": "Professional Female Host",
    "Cantonese_GentleLady": "Gentle Lady",
    "Cantonese_ProfessionalHost（M)": "Professional Male Host",
    "Cantonese_PlayfulMan": "Playful Man",
    "Cantonese_CuteGirl": "Cute Girl",
    "Cantonese_KindWoman": "Kind Woman",
}


def read_groups(reference):
    section = reference.split("## System Voices List (categorized by language)\n", 1)[1]
    section = section.split("\n## Voice Parameters", 1)[0]
    groups = {}
    group = None
    ids = set()
    for line in section.splitlines():
        heading = re.fullmatch(r"### (.+) Voices", line)
        if heading:
            group = heading[1]
            if group not in GROUPS or group in groups:
                raise ValueError(f"Unexpected or duplicate language group: {group}")
            groups[group] = []
        row = re.match(r"\| `([^`]+)` \| ([^|]+) \|", line)
        if row:
            voice_id, name = row[1], row[2].strip()
            if not group or voice_id in ids:
                raise ValueError(f"Ungrouped or duplicate voice: {voice_id}")
            ids.add(voice_id)
            groups[group].append((voice_id, name))
    if set(groups) != set(GROUPS) or not all(groups.values()):
        raise ValueError("Missing voice language groups")
    return groups


def catalog_text(groups):
    lines = ["/* Generated by scripts/sync_minimax_voices.py; do not edit.",
             f" * Source: {SOURCE_URL}",
             " * IDs and language groups are exact; UI names are English; defaults stay first.",
             " */"]
    for group, rows in groups.items():
        code, default_id = GROUPS[group]
        if default_id not in {voice_id for voice_id, _ in rows}:
            raise ValueError(f"Default voice missing from {group}: {default_id}")
        lines.extend(["", f"/* {group}: {len(rows)} voices. */"])
        for voice_id, name in sorted(rows, key=lambda row: row[0] != default_id):
            name = ENGLISH_NAMES.get(voice_id, name)
            if not name or not all(32 <= ord(character) < 127 for character in name):
                raise ValueError(f"English display name required for {voice_id}")
            args = ", ".join(json.dumps(value, ensure_ascii=False) for value in (voice_id, name, code))
            lines.append(f"VOICE({args}),")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path,
                        default=ROOT / "docs/references/minimax-voice-catalog.md")
    parser.add_argument("--check", action="store_true", help="Fail if the generated catalog differs")
    args = parser.parse_args()
    groups = read_groups(args.reference.read_text())
    generated = catalog_text(groups)
    catalog = ROOT / "main/minimax_voices.inc"
    if args.check:
        if catalog.read_text() != generated:
            raise SystemExit("MiniMax catalog differs from reference; run scripts/sync_minimax_voices.py")
    else:
        catalog.write_text(generated)
    print(f"PASS: {sum(map(len, groups.values()))} official MiniMax voices, {len(groups)} language groups")


if __name__ == "__main__":
    main()
