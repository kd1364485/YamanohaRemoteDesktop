use std::os::raw::{c_char, c_int};
//#[link(name = "yamanoha_core")] — yamanoha_core.dll にリンク
//extern "C" — C言語呼び出し規約。C++側は extern "C" でエクスポートしている
#[link(name = "yamanoha_core")]
extern "C" {
    pub(crate) fn yamanoha_init() -> c_int;
    pub(crate) fn yamanoha_shutdown();
    pub(crate) fn yamanoha_list_displays(
        out: *mut super::YamanohaDisplayInfo,
        max_count: c_int,
    ) -> c_int;

    pub(crate) fn yamanoha_set_callbacks(
        sdp_cb: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
        candidate_cb: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
        message_cb: Option<unsafe extern "C" fn(*const c_char)>,
        state_cb: Option<unsafe extern "C" fn(c_int)>,
    );

    pub(crate) fn yamanoha_start_stream(
        adapter_index: c_int,
        output_index: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate_kbps: c_int,
    ) -> c_int;

    pub(crate) fn yamanoha_switch_display(
        adapter_index: c_int,
        output_index: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate_kbps: c_int,
    ) -> c_int;

    pub(crate) fn yamanoha_stop_stream() -> c_int;

    pub(crate) fn yamanoha_process_signaling(
        msg_type: *const c_char,
        sdp_or_candidate: *const c_char,
        mid: *const c_char,
    ) -> c_int;
}
