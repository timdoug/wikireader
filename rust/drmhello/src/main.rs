//! drmhello - graphics from Rust on the WikiReader's panel: embedded-graphics
//! draws into a DRM dumb buffer (the drm crate) in the panel's own format,
//! D1 -- a bit a pixel, 1 black -- which the s1c33-lcdc driver scans where
//! it is.
//!
//!     openvt -c 2 -s -w -- drmhello.bin [seconds]
//!
//! The panel belongs to the foreground VT: on a VT of its own the console
//! steps aside and drops DRM master, and comes back when this exits.
//! [seconds] is how long to hold the picture, 5 by default.

use std::os::fd::{AsFd, BorrowedFd};
use std::{env, fs, thread, time};

use drm::buffer::{Buffer, DrmFourcc};
use drm::control::{Device as ControlDevice, connector, framebuffer};
use drm::Device;
use embedded_graphics::mono_font::{MonoTextStyle, ascii::FONT_10X20, ascii::FONT_6X10};
use embedded_graphics::pixelcolor::BinaryColor;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{Circle, Line, PrimitiveStyle, Rectangle};
use embedded_graphics::text::{Alignment, Text};

struct Card(fs::File);

impl AsFd for Card {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.0.as_fd()
    }
}

impl Device for Card {}
impl ControlDevice for Card {}

/// DRM_FORMAT_D1, fourcc 'D1  ': drm-fourcc has no name for it yet.
const FORMAT_D1: u32 = u32::from_le_bytes(*b"D1  ");

/// A mapped D1 buffer as an embedded-graphics target: eight pixels a byte,
/// the leftmost in the top bit, 1 black.
struct Canvas<'a> {
    pixels: &'a mut [u8],
    pitch: usize,
    size: Size,
}

impl OriginDimensions for Canvas<'_> {
    fn size(&self) -> Size {
        self.size
    }
}

impl DrawTarget for Canvas<'_> {
    type Color = BinaryColor;
    type Error = core::convert::Infallible;

    fn draw_iter<I>(&mut self, pixels: I) -> Result<(), Self::Error>
    where
        I: IntoIterator<Item = Pixel<BinaryColor>>,
    {
        for Pixel(p, colour) in pixels {
            if p.x < 0 || p.y < 0 || p.x as u32 >= self.size.width || p.y as u32 >= self.size.height {
                continue;
            }
            let at = p.y as usize * self.pitch + p.x as usize / 8;
            let bit = 0x80 >> (p.x % 8);
            if colour.is_on() {
                self.pixels[at] |= bit;
            } else {
                self.pixels[at] &= !bit;
            }
        }
        Ok(())
    }

    /// A row of a solid rectangle is whole bytes between two masked ends.
    /// clear() and the primitives' filled spans come here.
    fn fill_solid(&mut self, area: &Rectangle, colour: BinaryColor) -> Result<(), Self::Error> {
        let area = area.intersection(&self.bounding_box());
        let Some(bottom_right) = area.bottom_right() else {
            return Ok(());
        };
        let (x0, x1) = (area.top_left.x as usize, bottom_right.x as usize);
        let (first, last) = (x0 / 8, x1 / 8);
        let head = 0xffu8 >> (x0 % 8);
        let tail = 0xffu8 << (7 - x1 % 8);
        let fill = if colour.is_on() { 0xff } else { 0x00 };
        for y in area.top_left.y as usize..=bottom_right.y as usize {
            let row = &mut self.pixels[y * self.pitch..][..last + 1];
            let set = |byte: &mut u8, mask: u8| *byte = (*byte & !mask) | (fill & mask);
            if first == last {
                set(&mut row[first], head & tail);
            } else {
                set(&mut row[first], head);
                row[first + 1..last].fill(fill);
                set(&mut row[last], tail);
            }
        }
        Ok(())
    }
}

fn draw(canvas: &mut Canvas) {
    let size = canvas.size;
    let black = PrimitiveStyle::with_stroke(BinaryColor::On, 2);
    let filled = PrimitiveStyle::with_fill(BinaryColor::On);
    canvas.clear(BinaryColor::Off).unwrap();
    Rectangle::new(Point::new(2, 2), size - Size::new(4, 4))
        .into_styled(black)
        .draw(canvas)
        .unwrap();
    let centre = (size.width / 2) as i32;
    Text::with_alignment(
        "Hello, world!",
        Point::new(centre, 34),
        MonoTextStyle::new(&FONT_10X20, BinaryColor::On),
        Alignment::Center,
    )
    .draw(canvas)
    .unwrap();
    Text::with_alignment(
        "Rust on the C33, through DRM",
        Point::new(centre, 52),
        MonoTextStyle::new(&FONT_6X10, BinaryColor::On),
        Alignment::Center,
    )
    .draw(canvas)
    .unwrap();
    Circle::with_center(Point::new(centre - 60, 120), 70)
        .into_styled(black)
        .draw(canvas)
        .unwrap();
    Circle::with_center(Point::new(centre - 60, 120), 30)
        .into_styled(filled)
        .draw(canvas)
        .unwrap();
    // Three lines, not a stroked Triangle: embedded-graphics outlines a
    // triangle by intersecting its edges with every scanline, a third of a
    // second on this CPU; lines are Bresenham.
    let corners = [
        Point::new(centre + 25, 155),
        Point::new(centre + 95, 155),
        Point::new(centre + 60, 85),
    ];
    for i in 0..3 {
        Line::new(corners[i], corners[(i + 1) % 3])
            .into_styled(black)
            .draw(canvas)
            .unwrap();
    }
    for i in 0..8 {
        Rectangle::new(Point::new(centre - 80 + i * 20, 175), Size::new(12, 12))
            .into_styled(if i % 2 == 0 { filled } else { black })
            .draw(canvas)
            .unwrap();
    }
}

fn main() {
    let hold: u64 = env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(5);
    let card = Card(
        fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/dri/card0")
            .expect("open /dev/dri/card0"),
    );
    card.acquire_master_lock().expect("DRM master");

    let res = card.resource_handles().expect("resources");
    let con = res
        .connectors()
        .iter()
        .filter_map(|&c| card.get_connector(c, true).ok())
        .find(|c| c.state() == connector::State::Connected)
        .expect("no connected connector");
    let &mode = con.modes().first().expect("no mode");
    let crtc = res.crtcs().first().copied().expect("no CRTC");
    let (w, h) = mode.size();

    // The kernel sizes a dumb buffer by bits a pixel alone; the format given
    // here is only the crate's label for it.
    let mut db = card
        .create_dumb_buffer((w.into(), h.into()), DrmFourcc::C8, 1)
        .expect("dumb buffer");
    let pitch = db.pitch() as usize;
    let start = time::Instant::now();
    {
        let mut map = card.map_dumb_buffer(&mut db).expect("map");
        let mut canvas = Canvas {
            pixels: map.as_mut(),
            pitch,
            size: Size::new(w.into(), h.into()),
        };
        draw(&mut canvas);
    }
    let drawn = start.elapsed();
    let cmd = drm_ffi::mode::add_fb2(
        card.as_fd(),
        w.into(),
        h.into(),
        FORMAT_D1,
        &[db.handle().into(), 0, 0, 0],
        &[db.pitch(), 0, 0, 0],
        &[0; 4],
        &[0; 4],
        0,
    )
    .expect("D1 framebuffer");
    let fb: framebuffer::Handle = drm::control::from_u32(cmd.fb_id).expect("framebuffer id");
    card.set_crtc(crtc, Some(fb), (0, 0), &[con.handle()], Some(mode))
        .expect("set CRTC");
    println!("drmhello: {w}x{h} D1, drawn in {drawn:?}, shown in {:?}", start.elapsed());

    thread::sleep(time::Duration::from_secs(hold));
    card.destroy_framebuffer(fb).ok();
    card.destroy_dumb_buffer(db).ok();
    card.release_master_lock().ok();
}
