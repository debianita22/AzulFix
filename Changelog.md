AzulFix Changelog
=================
#### v1.1.3
- Turn the panel power sequence on when the panel framebuffer wakes (`doSetPowerState`), before Azul wakes the sink over AUX: powering it up just before `LightUpDP` left the picture black at S3 wake

#### v1.1.2
- Turn the panel power sequence on before `LightUpDP` trains the link, the backlight after it: powering the panel up after training broke the link (striped picture at S3 wake)

#### v1.1.1
- Wait the backlight off delay of the panel (`PP_OFF_DELAYS`) between the backlight off and `DisableDisplay`

#### v1.1.0
- Panel backlight on the DP panel port (DDI D): panel power sequence and backlight on after `LightUpDP`, backlight off before `DisableDisplay` (`-azulfixnopanel` to disable)

#### v1.0.0
- Initial release: `ASRDisable` = 1 in the `FeatureControl` read by `AppleIntelAzulController::getFeatureControl`, `findMEDevice` wait 10 s -> 1 ms
