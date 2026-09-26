const { parse, compileScript, compileTemplate, rewriteDefault } = require('@vue/compiler-sfc');
const babel = require('babel-jest').default.createTransformer();

// Use the application's Vue compiler for both setup code and real templates.
// Tests can override render when only the lifecycle needs to be exercised.
module.exports = {
    process(source, filename, options) {
        const { descriptor } = parse(source, { filename });
        const script = compileScript(descriptor, { id: filename });
        const template = compileTemplate({
            source: descriptor.template.content,
            filename,
            id: filename,
            compilerOptions: { bindingMetadata: script.bindings },
        });
        if (template.errors.length) throw new Error(template.errors.join('\n'));
        const code = `${rewriteDefault(script.content, '__component')}\n${template.code}
            __component.render = render;
            export default __component;`;
        return babel.process(code, filename, options);
    },
};
