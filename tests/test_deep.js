import * as os from 'os';
import * as std from 'std';
import inspect from 'inspect.so';
import * as deep from 'deep.so';
import * as xml from 'xml.so';
import Console from './console.js';

('use strict');
('use math');

globalThis.inspect = inspect;

function WriteFile(file, data) {
  let f = std.open(file, 'w+');
  f.puts(data);
  console.log(`Wrote '${file}': ${data.length} bytes`);
}

const inspectOptions = {
  colors: true,
  showHidden: false,
  customInspect: true,
  showProxy: false,
  getters: false,
  depth: Infinity,
  maxArrayLength: 100,
  maxStringLength: 200,
  compact: 2,
  hideKeys: ['loc', 'range', 'inspect', Symbol.for('nodejs.util.inspect.custom')]
};
function main(...args) {
  console = new Console(inspectOptions);

  console.log('args:', args);

  let data = std.loadFile(args[0] ?? 'FM-Radio-Receiver-1.5V.xml', 'utf-8');

  console.log('data:', data);

  let result = xml.read(data);
  console.log('result:', inspect(result, inspectOptions));

  let found = deep.find(result, n => typeof n == 'object' && n != null && n.tagName == 'elements');

  //deep.find(result, n => console.log(n));

  console.log('found:', inspect(found, inspectOptions));

  /* console.log('get:',
    inspect(deep.get(result, [2, 'children', 0, 'children', 3, 'children', 0]), inspectOptions)
  );
   console.log('set:',
    inspect(
      deep.set(result,
        [2, 'children', 0, 'children', 3, 'children', 1, 'children', 6, 'XXX', 'a', 'b', 'c', 'd'],
        'blah'
      ),
      inspectOptions
    )
  );
  console.log('get:',
    inspect(
      deep.get(result, [2, 'children', 0, 'children', 3, 'children', 1, 'children']),
      inspectOptions
    )
  );*/
  console.log('array:', inspect([, , , , 4, 5, 6, , ,], inspectOptions));
  let testObj = {};

  deep.set(testObj, 'a.0.b.0.c\\.x.0', null);
  deep.unset(testObj, 'a.0.b.0');
  console.log('testObj: ' + inspect(testObj, inspectOptions));


  let out = new Map();

  //out.set = function(...args) { console.log("args:", args); }
  //  out.set('@', ['blah']);

  let flat = deep.flatten(result, out, deep.MASK_PRIMITIVE | deep.MASK_STRING && ~deep.MASK_OBJECT);
  console.log('flat:', flat);
  console.log('flat.keys():', [...flat.keys()]);
  console.log('deep.MASK_STRING:', deep.MASK_NUMBER);
  console.log('deep:', deep);

  let clone = [];

  for(let [pointer, value] of out) {
    deep.set(clone, pointer, value);
  }

  let node = deep.get(result, '2.children.0.children.3.children.8.children.13.children.20');
console.log("get():", node);
  let path = deep.pathOf(result, node);
console.log("pathOf():", path);


let obj1 = {
  a: 1,b:2,c:3,d:4,e: [1,2,3,4,5]
};
let obj2 = {
  d:4,c:3,b:2,a:1,e: [1,2,3,4,5]
};


  console.log('equals():', deep.equals(obj1, obj2));

  std.gc();
}

main(...scriptArgs.slice(1));
