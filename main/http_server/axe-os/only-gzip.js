const fs = require('fs');
const path = require('path');

const directory = path.resolve(__dirname, 'dist/axe-os');

function keepOnlyCompressedFiles(currentDirectory) {
    for (const entry of fs.readdirSync(currentDirectory, { withFileTypes: true })) {
        const entryPath = path.join(currentDirectory, entry.name);
        if (entry.isDirectory()) {
            keepOnlyCompressedFiles(entryPath);
            if (fs.readdirSync(entryPath).length === 0) {
                fs.rmdirSync(entryPath);
            }
        } else if (!entry.name.endsWith('.gz')) {
            fs.unlinkSync(entryPath);
        }
    }
}

keepOnlyCompressedFiles(directory);
