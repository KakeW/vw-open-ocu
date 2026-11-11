# CAN and BAP evidence

The [MY2018 e-Golf reference](https://github.com/thomasakarlsen/e-golf-comfort-can/blob/5732e71f56af585ee846e76933c548d1ebeb2558/pages/the-comfort-canbus.md) describes 500 kbit/s Classic CAN and extended identifiers. Its [module communication notes](https://github.com/thomasakarlsen/e-golf-comfort-can/blob/5732e71f56af585ee846e76933c548d1ebeb2558/pages/how-modules-talk.md) distinguish broadcasts from BAP short, segmented and startup exchanges.

[SmartKar's BAP observations](https://github.com/karlsen-technologies/smartkar-cano-new/blob/2b191b8e9066d494125b3c0338787a347ec8d205/BAP_LONG_MESSAGE_FIX.md) describe the segmentation group and modulo-16 sequence. [Its OCU inventory](https://github.com/karlsen-technologies/smartkar-cano-new/blob/2b191b8e9066d494125b3c0338787a347ec8d205/docs/canbus-reverse-engineering/OCU_FEATURES.md) documents e-Golf climate, charging and telemetry candidates. Wake and startup are research topics only. The evidence does not establish Passat behavior.

Both repositories were accessible. The primary revision contains no license file and the SmartKar README explicitly withholds public-use licensing. This project references their observations and uses original synthetic fixtures.

The [registry](../protocol/compatibility/registry.json) records CANDIDATE for published e-Golf observations, CONFIRMED for independently verified Passat evidence, and UNKNOWN for observed but undecoded identifiers. Promote only with a repeatable capture and a documented validation method.
