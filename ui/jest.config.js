// jest.config.js
module.exports = {
    //...
    moduleNameMapper: {
        "^@/(.*)$": "<rootDir>/src/$1"
    },
    transform: {
        "^.+\\.vue$": "<rootDir>/tests/helpers/vueTransform.js",
        '^.+\\.(ts|tsx)?$': 'ts-jest',
        "^.+\\.(js|jsx)$": "babel-jest",
    },
    preset: 'ts-jest',
    //...
};
