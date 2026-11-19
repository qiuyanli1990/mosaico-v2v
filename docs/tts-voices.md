# MiniMax TTS voice selection

Aligned on 2026-11-05 with the [official MiniMax voice catalog](https://github.com/MiniMax-AI/skills/blob/main/skills/frontend-dev/references/minimax-voice-catalog.md).
The [checked-in reference snapshot](references/minimax-voice-catalog.md) contains the
full source, including every ID, name and language group.

The firmware imports **all 318 system voices in 24 language groups** from the
reference's **System Voices List (categorized by language)** section. The
recommendation sections are not additional voices: some recommendation IDs differ
from the full system tables. The system tables are the source of truth here.
IDs preserve the exact case, spaces and punctuation, including the full-width
parenthesis in `Cantonese_ProfessionalHost（F)` and `Cantonese_ProfessionalHost（M)`.

## Language coverage

All **312 voices across 23 groups** represented in the existing 60-language V2V
list are available in the target-language dropdown. The other six voices are
Cantonese: they are included under `yue` in the catalog, independently of Mandarin
`zh`, but are not exposed as a new V2V language. An accepted Cantonese ASR/translation
code must be established before enabling that route. No Cantonese voice is mapped
to Mandarin as a substitute.

The existing default for each group stays first; the remaining voices follow the
official table order. The UI uses **English display names for every language**.
The generator's `ENGLISH_NAMES` map translates the 50 Mandarin and 6 Cantonese
names, for example `male-qn-qingse` displays as `Youthful Man`. The other groups
retain the official English names. This affects labels only: all 318 raw IDs and
their language mappings remain exact. Generation fails if a voice lacks an
English display name. Both Live meeting steps, Travel and Media use these labels.

| Official group | Voice language | Count | Default raw ID |
| --- | --- | ---: | --- |
| Chinese Mandarin | `zh` | 50 | `male-qn-qingse` |
| Chinese Cantonese | `yue` | 6 | `Cantonese_ProfessionalHost（F)` |
| English | `en` | 16 | `English_Graceful_Lady` |
| Japanese | `ja` | 15 | `Japanese_GracefulMaiden` |
| Korean | `ko` | 49 | `Korean_CalmLady` |
| Spanish | `es` | 47 | `Spanish_SereneWoman` |
| Portuguese | `pt` | 73 | `Portuguese_SereneWoman` |
| French | `fr` | 5 | `French_Male_Speech_New` |
| Indonesian | `id` | 9 | `Indonesian_CalmWoman` |
| German | `de` | 3 | `German_FriendlyMan` |
| Russian | `ru` | 8 | `Russian_ReliableMan` |
| Italian | `it` | 4 | `Italian_Narrator` |
| Arabic | `ar` | 2 | `Arabic_CalmWoman` |
| Turkish | `tr` | 2 | `Turkish_CalmWoman` |
| Ukrainian | `uk` | 2 | `Ukrainian_CalmWoman` |
| Dutch | `nl` | 2 | `Dutch_kindhearted_girl` |
| Vietnamese | `vi` | 1 | `Vietnamese_kindhearted_girl` |
| Thai | `th` | 4 | `Thai_female_1_sample1` |
| Polish | `pl` | 4 | `Polish_female_1_sample1` |
| Romanian | `ro` | 4 | `Romanian_female_2_sample1` |
| Greek | `el` | 3 | `Greek_female_1_sample1` |
| Czech | `cs` | 3 | `czech_male_1_v1` |
| Finnish | `fi` | 3 | `finnish_male_3_v1` |
| Hindi | `hi` | 3 | `hindi_male_1_v2` |

## Corrections and languages without voices

The previous English choices contained three IDs absent from the supplied system
voice table: `English_Trustworth_Man`, `English_expressive_narrator` and
`English_CalmWoman`. The full English list now contains the table's 16 exact IDs,
including `English_Trustworthy_Man`, `English_Gentle-voiced_man` and `Serene_Woman`.

The catalog does not establish an English-voice mapping for Afrikaans, Bulgarian,
Catalan, Croatian, Danish, Hebrew, Hungarian, Malay, Norwegian, Slovak, Slovenian
or Swedish. These 12 V2V languages remain selectable, but their previous English
"multilingual" substitutions have been removed. They show `No voices available`
and send `tts.enable=false`, omitting the voice ID. This is a limit of the verified
voice mapping; it does not claim that MiniMax cannot synthesize those languages.
The project currently displays no translated subtitles, so these targets have no
spoken or visible translation output.

Singapore English and Mandarin use the `en` and `zh` groups. Matching accepts
`en-SG` and `zh-SG`; the existing V2V requests continue to use `en-US` and `zh-CN`.
Malay has no published voice group. Filipino remains without spoken output on
the current speech-02 route. Burmese is absent from the existing V2V list and
from the supplied voice catalog.

## Request format and selection

The Agora V2V request uses exactly one existing provider prefix:

```text
English_Trustworthy_Man -> fm_02_English_Trustworthy_Man
Chinese (Mandarin)_Warm_Bestie -> fm_02_Chinese (Mandarin)_Warm_Bestie
```

Both the selection API and JSON builder accept raw or already-prefixed IDs.
Empty IDs, repeated prefixes and IDs that do not fit the bounded request buffer
are rejected. TTS speed remains 1.3.

The dropdown is rebuilt when the target language changes. In Media, the room uses
the selected target-language voice. Live meeting uses two independent setup pages:
**Media to you** selects the USB playback channel's source language, target language
and voice; **You to media** selects the microphone channel's source, target and
voice. Each Agent request uses its own page's settings. The first visit to the
microphone step defaults to the opposite language pair, and later visits preserve
the selections. Travel retains the automatic opposite language pair and default
voice for its reverse room. A stale voice from a different target language falls
back to that target's default in either channel.

The dropdown buffer is 1536 bytes; the longest current list is Portuguese with
73 entries, requiring 1028 bytes including separators and the terminator.

## Updating and validation

`main/minimax_voices.inc` is generated from the checked-in reference:

```sh
python3 scripts/sync_minimax_voices.py
python3 scripts/sync_minimax_voices.py --check
```

To update, replace the snapshot with the latest official file, regenerate, and
review the changes. Add English labels to `ENGLISH_NAMES` for any new voices
whose official display names are not in English. No additional font is required.

Host tests compare all 318 generated entries against the reference, check group
counts, ID formatting, unsupported targets, defaults and bidirectional session
selection. The actual LVGL test selects the final item of every supported list,
checks the full option counts and English labels for Chinese voices in both Live
meeting steps, and renders the Chinese and Portuguese voice dropdowns. Host tests
check printable ASCII display names for all 318 entries. These checks do not replace listening tests or verify
that every voice is enabled on the configured cloud TTS Worker.
