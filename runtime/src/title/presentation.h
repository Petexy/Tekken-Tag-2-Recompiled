#pragma once

// How the title presents buttons and its logo on a computer:
// - Menus confirm with the bottom face button and go back with the right
//   one, as on the other consoles (Nintendo controllers keep the Wii U's
//   right-button confirm). The title has this choice built in, from its
//   PlayStation 3 version's system setting; its icons follow it.
// - Button icons follow the controller in use: PlayStation controllers get
//   PlayStation-style icons drawn here, others neutral ones (face buttons
//   shown by their place on the pad). Both are made from the title's font.
// - The title screen's logo and copyright line lose "Wii U Edition".
// Everything is made from the Wii U game's own files.

namespace cafe::title {

// After vfs::open_game and the image load, before the title runs.
void prepare_presentation();

} // namespace cafe::title
