const { parse, compileScript } = require('@vue/compiler-sfc');
const babel = require('babel-jest').default.createTransformer();

// Compile setup scripts with the same Vue compiler as the application. The
// lifecycle tests mount them with a null render; Vite validates the templates.
module.exports = {
    process(source, filename, options) {
        const { descriptor } = parse(source, { filename });
        const script = compileScript(descriptor, { id: filename });
        return babel.process(script.content, filename, options);
    },
};
