# MiniMax TTS voice selection

Reviewed against the public MiniMax documentation on 2026-10-29:

- [System voice IDs (international)](https://platform.minimax.io/docs/faq/system-voice-id)
- [System voice IDs (Chinese, including the existing Qingse voice)](https://platform.minimaxi.com/docs/faq/system-voice-id)
- [Text-to-audio API and model language restrictions](https://platform.minimax.io/docs/api-reference/speech-t2a-http)

The firmware keeps a small selection for general conversation and narration rather
than importing all 300+ system voices. It contains 55 distinct official voice IDs:
55 native-language options across 23 language groups, plus two of the same English
voices as explicit multilingual options for 12 other supported languages. This is
57 catalog entries, covering all 35 TTS-enabled languages in the existing V2V list.
The first entry for each language is its default.

## Request format and matching

The IDs in the table below are the exact raw MiniMax IDs, including case, spaces
and punctuation. The Agora V2V request uses the existing provider prefix, for example:

```text
English_Graceful_Lady -> fm_02_English_Graceful_Lady
Chinese (Mandarin)_Warm_Bestie -> fm_02_Chinese (Mandarin)_Warm_Bestie
```

Both the selection API and JSON builder accept raw or already-prefixed IDs and
produce exactly one `fm_02_` prefix. Empty IDs, repeated prefixes and IDs that do
not fit the bounded request buffer are rejected. TTS speed remains 1.3.

The voice dropdown is rebuilt from the **target** language when that language
changes. A native voice is not offered for an unrelated language. In Media, the
single room uses the selected target-language voice. In Live meeting and Travel,
the forward room uses that choice; the reverse room uses the first voice matching
its own target language. A stale selection from a different target language falls
back to that target's first voice.

## Native voice choices

| Language | Choices, in default order (raw ID: display name) |
| --- | --- |
| Chinese | `male-qn-qingse`: Qingse (male)<br>`Chinese (Mandarin)_Warm_Bestie`: Warm Bestie (female)<br>`Chinese (Mandarin)_Gentleman`: Gentleman (male)<br>`Chinese (Mandarin)_News_Anchor`: News Anchor<br>`Chinese (Mandarin)_Sweet_Lady`: Sweet Lady |
| English | `English_Graceful_Lady`: Graceful Lady (female)<br>`English_Trustworth_Man`: Trustworthy Man<br>`English_expressive_narrator`: Expressive Narrator<br>`English_CalmWoman`: Calm Woman |
| Japanese | `Japanese_GracefulMaiden`: Graceful Maiden<br>`Japanese_GentleButler`: Gentle Butler (male)<br>`Japanese_CalmLady`: Calm Lady |
| Korean | `Korean_CalmLady`: Calm Lady<br>`Korean_CalmGentleman`: Calm Gentleman<br>`Korean_IntellectualMan`: Intellectual Man |
| German | `German_FriendlyMan`: Friendly Man<br>`German_SweetLady`: Sweet Lady |
| French | `French_Male_Speech_New`: Level-Headed Man<br>`French_Female_News Anchor`: Patient Presenter (female) |
| Spanish | `Spanish_SereneWoman`: Serene Woman<br>`Spanish_ThoughtfulMan`: Thoughtful Man<br>`Spanish_Narrator`: Narrator |
| Portuguese | `Portuguese_SereneWoman`: Serene Woman<br>`Portuguese_ReliableMan`: Reliable Man |
| Russian | `Russian_ReliableMan`: Reliable Man<br>`Russian_AmbitiousWoman`: Ambitious Woman |
| Italian | `Italian_Narrator`: Narrator<br>`Italian_BraveHeroine`: Brave Heroine |
| Arabic | `Arabic_CalmWoman`: Calm Woman<br>`Arabic_FriendlyGuy`: Friendly Guy |
| Hindi | `hindi_male_1_v2`: Trustworthy Advisor (male)<br>`hindi_female_2_v1`: Tranquil Woman |
| Indonesian | `Indonesian_CalmWoman`: Calm Woman<br>`Indonesian_CaringMan`: Caring Man<br>`Indonesian_GentleGirl`: Gentle Girl |
| Vietnamese | `Vietnamese_kindhearted_girl`: Kind-Hearted Girl |
| Thai | `Thai_female_1_sample1`: Confident Woman<br>`Thai_male_1_sample8`: Serene Man<br>`Thai_male_2_sample2`: Friendly Man |
| Dutch | `Dutch_kindhearted_girl`: Kind-Hearted Girl<br>`Dutch_bossy_leader`: Bossy Leader |
| Turkish | `Turkish_CalmWoman`: Calm Woman<br>`Turkish_Trustworthyman`: Trustworthy Man |
| Ukrainian | `Ukrainian_CalmWoman`: Calm Woman<br>`Ukrainian_WiseScholar`: Wise Scholar |
| Polish | `Polish_female_1_sample1`: Calm Woman<br>`Polish_male_1_sample4`: Male Narrator |
| Romanian | `Romanian_female_2_sample1`: Gentle Woman<br>`Romanian_male_1_sample2`: Reliable Man |
| Greek | `Greek_female_1_sample1`: Gentle Lady<br>`greek_male_1a_v1`: Thoughtful Mentor |
| Czech | `czech_male_1_v1`: Assured Presenter<br>`czech_female_2_v2`: Elegant Lady |
| Finnish | `finnish_male_3_v1`: Upbeat Man<br>`finnish_female_4_v1`: Assertive Woman |

## Multilingual choices and Southeast Asia

Afrikaans, Bulgarian, Catalan, Croatian, Danish, Hebrew, Hungarian, Malay, Norwegian,
Slovak, Slovenian and Swedish use `English_Graceful_Lady` and
`English_Trustworth_Man`, labelled **multilingual** in the dropdown. MiniMax lists
these synthesis languages but publishes no dedicated native voice group for them.
These choices do not imply a native regional accent. Cloud account availability,
voice access and pronunciation still need end-to-end service validation.

| Region / language | Firmware behavior |
| --- | --- |
| Singapore | Choose English, Chinese or Malay. There is no dedicated Singapore voice in the published catalog. Voice matching recognizes `en-SG`, `zh-SG` and `ms-SG` by language, but requests continue using the existing supported V2V codes `en-US`, `zh-CN` and `ms-MY`. |
| Indonesia | Three Indonesian-native voices. |
| Vietnam | The one published Vietnamese-native voice, `Vietnamese_kindhearted_girl`. |
| Thailand | Three Thai-native voices, including female and male choices. |
| Malaysia / Malay | Two explicitly labelled multilingual choices. |
| Philippines / Filipino (Tagalog) | The existing `tl-PH` V2V language remains selectable, but returned text is not displayed in the UI. Spoken output stays disabled on the existing speech-02 path: MiniMax explicitly excludes Filipino from speech-01/02. Newer models list Filipino, but the current V2V provider/model route has not been verified with them. No fabricated Filipino-native voice is offered. |
| Myanmar / Burmese | Absent from both the current V2V language list and MiniMax's published TTS language/voice list. No Burmese option or substitute voice is advertised. This requires documented service support before it can be enabled. |

The existing 60 V2V language options and 35-language TTS boundary are retained.
Targets without TTS support send `tts.enable=false` and omit the voice ID. Live
meeting and Media show fixed connection instructions; no mode displays subtitles.
Those targets cannot produce a spoken translation on the current TTS path.

## Validation

All 55 distinct raw IDs were compared with exact cells from the official voice
tables above. Host tests cover canonical prefix generation, catalog coverage,
target-language selection, reverse-direction defaults, unsupported targets and
bounded dropdown/request buffers. The UI test renders the real LVGL pages.
These checks do not replace listening tests with the configured V2V service.
