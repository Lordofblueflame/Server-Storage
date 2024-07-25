import data from './data.json' assert { type: 'json' };

console.log(data);

function displayTree(data, parentElement) {
    const ul = document.createElement('ul');

    data.files.forEach(file => {
        const li = document.createElement('li');
        li.textContent = file.path;
        ul.appendChild(li);
    });

    for (const [dirName, subDirData] of Object.entries(data.subdirectories)) {
        const li = document.createElement('li');
        li.textContent = dirName;
        li.classList.add('folder');

        const subDirUl = document.createElement('ul');
        displayTree(subDirData, subDirUl);
        li.appendChild(subDirUl);

        ul.appendChild(li);
    }

    parentElement.appendChild(ul);
}

document.addEventListener('DOMContentLoaded', () => {
    const container = document.getElementById('fileTree');
    displayTree(data, container);
});

function handleUpload() {
    alert('Upload functionality is not implemented yet.');
}

function handleDownload() {
    alert('Download functionality is not implemented yet.');
}
