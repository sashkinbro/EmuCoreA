// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

package com.sbro.emucorea.xmb

/**
 * The images the renderer loads by path, with the SHA-256 of each one's source
 * payload in the 6.60 firmware (a digest, not the image).
 *
 * Entries of `topmenu_icon.rco` have positional labels (`EV`, `FA` ...) that shift
 * between firmware versions, so after extraction [XmbResources] places those by
 * digest. A positional image no extracted file matches belongs to a feature the
 * firmware doesn't have: its path is cleared and the renderer hides that entry.
 * The [Expected.required] ones (category icons, the Game column) must exist, or
 * the firmware is rejected. Images with real labels (`tex_*`) keep whatever the
 * firmware has, since a different digest there is just that version's artwork.
 *
 * Generated from the 6.60 reference manifest; regenerate when the renderer
 * starts loading another image.
 */
internal object XmbExpectedImages {
    data class Expected(val sha256: String, val positional: Boolean, val required: Boolean)

    val byPath: Map<String, Expected> = mapOf(
        "images/game_plugin/tex_arrow_l.png" to Expected("96da0a9041bab5ee4685fbd71f0eab56bdbf4738534608fe5025982768d74aad", false, false),
        "images/game_plugin/tex_arrow_l_shadow.png" to Expected("087907fec90be4322c00eb9d960ff99a97cf8b0073ce55c24f3fe21125dda6a9", false, false),
        "images/game_plugin/tex_arrow_r.png" to Expected("2c3b33d7ae6ff7fe7d28b7203d7b3c2ec2a72163eeb1935de76e6d93e082cdaf", false, false),
        "images/game_plugin/tex_arrow_r_shadow.png" to Expected("cc070dc8ff8568ad1b2dafbba1dcd057e2aa481e724dc1527408deb115819a94", false, false),
        "images/game_plugin/tex_subs_frame.png" to Expected("6b6a6719aa04c98cada37760bc789201050640e744b9f8a8fae452abd3958f0e", false, false),
        "images/impose_plugin/tex_avls.png" to Expected("6055d245aef8742abed362d153a4cc781a78e24afeaf3b3179f76eb5435c4781", false, false),
        "images/impose_plugin/tex_bt_mark.png" to Expected("8b09e825ef31673f1fe208c8f4bdd82b9dc802757ae437c3ad62b9de65275048", false, false),
        "images/netconf_dialog/tex_netconf_icon.png" to Expected("aae7731d02b10487daf0267e4cc3454608aa4ed4f54cf47f19bfaa47702f7f9a", false, false),
        "images/opening_plugin/tex_glow.png" to Expected("64a3d8ad5b4d07d89f17dc4914b83bf5065a877e47b65eeee1ac6489ac82770d", false, false),
        "images/opening_plugin/tex_glow_A.png" to Expected("19e838910b316bbd57a399ab58eed4ed92b276390293abffe34fa2cff04ff4db", false, false),
        "images/opening_plugin/tex_glow_B_buttom.png" to Expected("b8810e1fe31730e5dec05ded630104535c6b5ab13067f22922d7e71f20226b73", false, false),
        "images/opening_plugin/tex_glow_B_mid.png" to Expected("6084c2a348c56006f5e81e310c5a6849b478bd8694be42de50241095b6282782", false, false),
        "images/opening_plugin/tex_glow_B_top.png" to Expected("fcc25b027c768d6d2bcd4d624ce819dd620ac74e8b6362eba3da35e36f9a2059", false, false),
        "images/savedata_plugin/icon_loading.png" to Expected("ee92099df93f727d8d88aab6604e8fe836e520929e401bf612b71d51e1dc247e", false, false),
        "images/sysconf_plugin/tex_classic_preview.png" to Expected("37a952c4de18c9f420184bb67b4bba4304483dd8ab9c6fe41ba912430083428a", false, false),
        "images/sysconf_plugin/tex_ms.png" to Expected("3267b2f0f53ef92bdfd19ac8781dadd0c936563f89d6ff4c4094e6bc71245bea", false, false),
        "images/sysconf_plugin/tex_original_preview.png" to Expected("cda1779f1cafc5676871d3240705c0e07e7d943aa55529b7510f07dad3b8c1df", false, false),
        "images/system_plugin/tex_3x3_focus.png" to Expected("d7ee09e49cffa1bc8e52390b50cc15c7f1ef8505f1811f783335b6f33fbf7126", false, false),
        "images/system_plugin/tex_default_game_icon.png" to Expected("850f5a84f136bca0b30a266fc721bc6d6554a1444d5816663dfd37679eeb63b1", false, false),
        "images/system_plugin_fg/tex_battery.png" to Expected("b1835fbcf708b8aca77d48b521c42de3ccaf9900322b44c621ba71f5e225a847", false, false),
        "images/system_plugin_fg/tex_battery_shadow.png" to Expected("b2e8335b302c72f049865071f07062b3ec119d7c306a19557501388f730d9a30", false, false),
        "images/system_plugin_fg/tex_busy.png" to Expected("b19c0ce8e5e2e37f3a0145d891592acbfba887e7cb808670e970178ba82f1b78", false, false),
        "images/system_plugin_fg/tex_busy_shadow.png" to Expected("5a1a293c6484baaea529209c8268115d330cd40e968d62d65f16a89473041ba1", false, false),
        "images/system_plugin_fg/tex_circle.png" to Expected("05b1f7bb2a4c0e319738295930e84ca06da94f2079c90194479737e3342474e3", false, false),
        "images/system_plugin_fg/tex_cross.png" to Expected("04ac62d261e864eacb84e8c325d736ad9db90a25a4c73c77d01eab91416c6611", false, false),
        "images/system_plugin_fg/tex_mute.png" to Expected("90ecb024f4032720724f05bf7420b830236224fee677dde65a78d49ae51aa8de", false, false),
        "images/system_plugin_fg/tex_mute_shadow.png" to Expected("f325afb65e5f488142cfbac9a4600c46f657a472e83f4211b1ba1fa16b990c22", false, false),
        "images/system_plugin_fg/tex_square.png" to Expected("f0258c6b40c85020267dbdfc9ce1b41585410ef2d4124c5194bb4e51a3170677", false, false),
        "images/system_plugin_fg/tex_triangle.png" to Expected("1347af994bfc56f670ccd6ceddb905400fad6de75a4001dd8872046b5e485d28", false, false),
        "images/topmenu_icon/AJ.png" to Expected("ffad35b4c349da0d25d9c1e289e364e1ab323c6b64fcd480f23d491b18021513", true, true),
        "images/topmenu_icon/AK.png" to Expected("b7799a6b6c7580e9bc6cc1aef6a1b9d57a7fa45dad37114ebcc1fdc1eb609c1e", true, true),
        "images/topmenu_icon/AL.png" to Expected("d23ec09cb1de957ba108d0afcd801d1820daf1d5575064d69654e307ae0c08ca", true, true),
        "images/topmenu_icon/AM.png" to Expected("d3ba21efe1f5939bff06e181efab863f95fffd6de8976d13440d7a35eebc740e", true, true),
        "images/topmenu_icon/AN.png" to Expected("f07cd32f73c94eeb7ea1390b2e967396f5a0bd2e0aa7742db4a100dc4280010a", true, true),
        "images/topmenu_icon/AO.png" to Expected("91d9583d9d9be4bca82e7410e07686d6d53f504c10a2d0de003a7ed4038b4a84", true, true),
        "images/topmenu_icon/AP.png" to Expected("36e60bd0f83dfcc5ef766774c6734926cd76b392a23cec389de9d909b7b7c025", true, true),
        "images/topmenu_icon/AQ.png" to Expected("6f5bc0d95181a0cdc715f41e8b8f5498650f3044fb1100edca710a36125a0f59", true, true),
        "images/topmenu_icon/BI.png" to Expected("cbf8324aad0ef21931b04fec51b295d09ea6285c31429637571f8717c4eb0b1e", true, true),
        "images/topmenu_icon/BT.png" to Expected("bc36d559aa9b182b397291fbea49edb4e283e4824b97a0f643128e2a142989c2", true, false),
        "images/topmenu_icon/BU.png" to Expected("c83ee392f5f8c83cf169494b842d30e086fdab8209c1cd0cefaeac3b6cf1a713", true, false),
        "images/topmenu_icon/BV.png" to Expected("97a249af229a9c5444873de7b83bba798cec35bbbc4ded83c8ce7ad5e920f9c6", true, false),
        "images/topmenu_icon/BW.png" to Expected("18a43c46614d7e8cf0e75f75084ee3d72def50dd427a5ea8a0e12f5ae5ed48de", true, false),
        "images/topmenu_icon/BX.png" to Expected("4c246c08588525fbb937b14ddf1f3fe855066caf7e1af4acdc308ff7e90757f4", true, false),
        "images/topmenu_icon/BY.png" to Expected("818f83dff5cda20aa7096ced3f5188d32a344f2ad1c0f308943b0a58c5171eb3", true, false),
        "images/topmenu_icon/BZ.png" to Expected("e28ae4c7a7a8c680d80f3dbd0792e9a245087b1f1a3f9a17cd5891442df846cc", true, false),
        "images/topmenu_icon/CA.png" to Expected("1d8dcaddc1edb17495a79b2c7f02d33657529532135adfd38950937d16f661cb", true, false),
        "images/topmenu_icon/CB.png" to Expected("a49392ea8c5a227b4ec2e7cbd46cd87f4a8986385934cb663965d786be2eea6e", true, false),
        "images/topmenu_icon/CC.png" to Expected("80723a42e33b5b1e638ef205ff6ed19476415fda371b68df287f8150d9059d63", true, false),
        "images/topmenu_icon/CD.png" to Expected("46cda0f16d2f08eacc3cf029b8b9fa4b8c7064f3d0fd4f37fbd166bb89abe2c0", true, false),
        "images/topmenu_icon/CE.png" to Expected("ead2b329b39a8661470e0f2dc333f5088158b9a83f9d68c245ac74834cfd35ba", true, false),
        "images/topmenu_icon/CF.png" to Expected("020ab9744675a27df71e28c34fc306aa9c49f89a4e8651c07c70e7527b826cd7", true, false),
        "images/topmenu_icon/CG.png" to Expected("1cb10ae36aa286e653ceb081f983e2648ee5b461494080759605472a654252b8", true, false),
        "images/topmenu_icon/DJ.png" to Expected("80a25ef48096b6d66524824fa848bfed031fb141c334cdc56bc4bc91614ce60b", true, false),
        "images/topmenu_icon/DM.png" to Expected("dfbb93769bc1acc33a51beb854877f2436240642cc1ab2a776f50a4a7f8c5b6b", true, false),
        "images/topmenu_icon/DN.png" to Expected("b9b117a54e65a4d33f1be547bbe94d4ba23295836cd18fff54667e0f39e7d2f6", true, false),
        "images/topmenu_icon/DS.png" to Expected("8a92141e8ba8a7100d53fc21d44a21d8001ccef3ac2b910489206be9d7daf899", true, false),
        "images/topmenu_icon/DV.png" to Expected("20314bca6178f76d2cd0d8012e54d8bfa0ba1febcb683e83b62065ae1f2cffe5", true, false),
        "images/topmenu_icon/EI.png" to Expected("a325d62f8f8283f51e710d8424fbf1a4db4b262cf5c8f934e4f78aa506bb0348", true, true),
        "images/topmenu_icon/EJ.png" to Expected("b94d1d4f3ef3b8e792a857543193180663154ce12ead775851525a25866d49a5", true, true),
        "images/topmenu_icon/ET.png" to Expected("ee2264c6971b241b67c569702a26f1d3d3a48e3fb656a40a8ab6abc3a903ff78", true, false),
        "images/topmenu_icon/EU.png" to Expected("6baac9ee58d5f48c91681057b3bd9d9c6c37cf3ed0d0f7272d78286a7f13d57a", true, false),
        "images/topmenu_icon/EV.png" to Expected("c8dff1694bd570756f6674db0e1aff841e9c7b88f86bff7ba37195a12649bee7", true, false),
        "images/topmenu_icon/EW.png" to Expected("1c8afc44f955d10b2e566bbd319a580bf017fb27e37fa86bb7d8565390041e19", true, false),
        "images/topmenu_icon/EX.png" to Expected("d6120caa35193bc9f91670458dd929c09e71ec868a6f26cf60316ecd7bc1c00d", true, false),
        "images/topmenu_icon/EZ.png" to Expected("74dcf31cb838136fa5de4ca9aed9a684c550514e33f313ff6a74a5f1537dba4a", true, false),
        "images/topmenu_icon/FA.png" to Expected("83ec30ab64dc6c8102832729370e898ae67cc6a806eb28f53515c6241394ff35", true, false),
        "images/topmenu_icon/FB.png" to Expected("17e392148d1a257bcd459989a3f2e2d940a71ab8213b783d4045a3bc83a60148", true, false),
        "images/topmenu_icon/FV.png" to Expected("55ed20fa041cdc5f03ccc71a114962e088d900b908a6f796b02115ac40ba8695", true, false),
        "images/topmenu_icon/FW.png" to Expected("a9a7871afeedcaf72b3eae3fc841d9db01e9175f9419d415b206e0c0bc9d1655", true, false),
        "images/topmenu_icon/FX.png" to Expected("e269ae988c2ffad20e57915026413d715bb3945f0bf31ad72b13f043f4e92e62", true, false),
    )
}
