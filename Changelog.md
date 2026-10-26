AzulFix Changelog
=================
#### v1.1.1
- Wait the backlight off delay of the panel (`PP_OFF_DELAYS`) between the backlight off and `DisableDisplay`

#### v1.1.0
- Panel backlight on the DP panel port (DDI D): panel power sequence and backlight on after `LightUpDP`, backlight off before `DisableDisplay` (`-azulfixnopanel` to disable)

#### v1.0.0
- Initial release: `ASRDisable` = 1 in the `FeatureControl` read by `AppleIntelAzulController::getFeatureControl`, `findMEDevice` wait 10 s -> 1 ms
