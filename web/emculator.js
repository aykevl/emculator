'use strict';

const PAGESIZE     = 1024;
const IMAGESIZE    = 256 * 1024;
const RAMSIZE      = 32 * 1024;
const FIRMWARE_URL = 'firmware.bin';

var memory = new WebAssembly.Memory({ initial: 32, maximum: 32 });
var memoryStart = 0; // like sbrk(), used by wasm_malloc() for allocation

var emculator;
var machineInstance; // *machine_t
var image8, image32; // two views on the loaded firmware image

var importObject = {
  imports: {
    imported_func: arg => console.log(arg),
  },
  env: {
    memory: memory,
    _wasm_malloc: function(size) {
      let buf = memoryStart;
      if ((size & 7) != 0) {
        size += (size & 7); // align on 8-byte boundary
      }
      memoryStart += size;
      return buf;
    },
    _terminal_getchar: function() {
      throw 'TODO: terminal_getchar';
    },
    _terminal_putchar: function(c) {
      document.querySelector('#terminal').textContent += String.fromCharCode(c);
    },
  },
};


function init() {
  // Do two things parallel:
  // 1. Fetch and instantiate the machine module.
  // 2. Fetch the firmware file.
  // After that, write the firmware image to the machine and start it.
  Promise.all([
    // Fetch and instantiate machine.
    WebAssembly.instantiateStreaming(fetch('machine.wasm'), importObject).then(function(obj) {
      emculator = obj.instance;
      machineInstance = emculator.exports._machine_create(IMAGESIZE, PAGESIZE, RAMSIZE, 0);
      let imageAddress = emculator.exports._machine_get_image(machineInstance);
      image8 = new Uint8Array(memory.buffer, imageAddress, IMAGESIZE);
      image32 = new Uint32Array(memory.buffer, imageAddress, IMAGESIZE);
    }),
    // Load the firmware image.
    fetch(FIRMWARE_URL).then(function(response) {
      if (!response.ok) {
        throw 'Cannot fetch firmware: HTTP error ' + response.status;
      }
      return response.arrayBuffer();
    })]).then(function(array) {
      // Write the firmware image to the emulated chip.
      image8.set(new Uint8Array(array[1]), 0);
      // Reset: initialize PC and SP.
      emculator.exports._machine_reset(machineInstance);
      // Start the emulator.
      let err = emculator.exports._machine_run(machineInstance);
      console.log('emculator result:', err);
    }).catch(function(err) {
      console.error(err);
    });
}

init();
