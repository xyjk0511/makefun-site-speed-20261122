if(process.platform==='linux'&&process.arch==='x64')process.dlopen({exports:{}},__dirname+'/x64.node');else import('./fallback.mjs');
