mod bindings;
mod callbacks;
mod wrappers;

use std::os::raw::c_int;

#[repr(C)]
#[derive(Debug, Clone)]
pub struct YamanohaDisplayInfo {
    pub name: [u16; 128],
    pub adapter_name: [u16; 128],
    pub adapter_index: c_int,
    pub output_index: c_int,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub refresh_rate: f32,
    pub is_primary: c_int,
}

impl Default for YamanohaDisplayInfo {
    fn default() -> Self {
        Self {
            name: [0; 128],
            adapter_name: [0; 128],
            adapter_index: 0,
            output_index: 0,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            refresh_rate: 0.0,
            is_primary: 0,
        }
    }
}

pub use callbacks::WS_SENDER;
pub use wrappers::*;

pub fn handle_remote_input(message: &str) {
    crate::input_control::handle_remote_input(message);
}
