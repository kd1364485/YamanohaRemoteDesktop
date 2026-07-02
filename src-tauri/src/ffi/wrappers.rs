use std::ffi::CString;
use std::os::raw::c_int;
use tauri::AppHandle;

use super::bindings::*;
use super::callbacks;

pub fn init_engine(app: AppHandle) -> Result<(), String> {
    *callbacks::TAURI_APP.lock().unwrap() = Some(app);

    let res = unsafe { yamanoha_init() };
    if res != 0 {
        return Err(format!("Engine initialization failed: {}", res));
    }

    unsafe {
        yamanoha_set_callbacks(
            Some(callbacks::on_local_description),
            Some(callbacks::on_local_candidate),
            Some(callbacks::on_remote_message),
            Some(callbacks::on_state_change),
        );
    }

    Ok(())
}

pub fn shutdown_engine() {
    unsafe { yamanoha_shutdown() };
    *callbacks::TAURI_APP.lock().unwrap() = None;
}

pub fn list_displays() -> Result<Vec<super::YamanohaDisplayInfo>, String> {
    let mut buffer = vec![super::YamanohaDisplayInfo::default(); 10];

    let count = unsafe { yamanoha_list_displays(buffer.as_mut_ptr(), buffer.len() as c_int) };
    if count < 0 {
        return Err(format!("Failed to list displays: {}", count));
    }

    buffer.truncate(count as usize);
    Ok(buffer)
}

pub fn start_stream(
    adapter_index: i32,
    output_index: i32,
    width: i32,
    height: i32,
    fps: i32,
    bitrate_kbps: i32,
) -> Result<(), String> {
    let res = unsafe {
        yamanoha_start_stream(
            adapter_index as c_int,
            output_index as c_int,
            width as c_int,
            height as c_int,
            fps as c_int,
            bitrate_kbps as c_int,
        )
    };
    if res != 0 {
        return Err(format!("Failed to start stream: {}", res));
    }
    Ok(())
}

pub fn stop_stream() -> Result<(), String> {
    let res = unsafe { yamanoha_stop_stream() };
    if res != 0 {
        return Err(format!("Failed to stop stream: {}", res));
    }
    Ok(())
}

pub fn switch_display(
    adapter_index: i32,
    output_index: i32,
    width: i32,
    height: i32,
    fps: i32,
    bitrate_kbps: i32,
) -> Result<(), String> {
    let res = unsafe {
        yamanoha_switch_display(
            adapter_index as c_int,
            output_index as c_int,
            width as c_int,
            height as c_int,
            fps as c_int,
            bitrate_kbps as c_int,
        )
    };
    if res != 0 {
        return Err(format!("Failed to switch display: {}", res));
    }
    Ok(())
}

pub fn process_signaling(
    msg_type: &str,
    sdp_or_candidate: &str,
    mid: Option<&str>,
) -> Result<(), String> {
    let c_msg_type = CString::new(msg_type).map_err(|e| e.to_string())?;
    let c_sdp_or_candidate = CString::new(sdp_or_candidate).map_err(|e| e.to_string())?;

    let c_mid = match mid {
        Some(m) => Some(CString::new(m).map_err(|e| e.to_string())?),
        None => None,
    };

    let c_mid_ptr = match &c_mid {
        Some(m) => m.as_ptr(),
        None => std::ptr::null(),
    };

    let res = unsafe {
        yamanoha_process_signaling(c_msg_type.as_ptr(), c_sdp_or_candidate.as_ptr(), c_mid_ptr)
    };

    if res == 0 {
        return Err("Failed to process signaling message".to_string());
    }
    Ok(())
}
