"use strict";
let container = null;
let currentDirElement = null;
let backButton = null;
let currentData = null;
let historyStack = [];
let initialData = null;
function updateTree(data, parentElement, currentDirElement, backButtonElement) {
    if (!data || typeof data !== 'object') {
        return;
    }
    currentData = data;
    displayTree(currentData, parentElement, currentDirElement);
    if (backButtonElement) {
        if (historyStack.length > 1) {
            backButtonElement.style.display = 'inline-block';
            backButtonElement.classList.add('active');
        }
        else {
            backButtonElement.style.display = 'none';
            backButtonElement.classList.remove('active');
        }
    }
    else {
        console.error('backButtonElement is null');
    }
}
function handleUpload() {
    const fileInput = document.getElementById('file-input');
    fileInput.click();
}
function displayTree(data, parentElement, currentDirElement) {
    parentElement.innerHTML = '';
    currentDirElement.textContent = `Current dir: ${data.path}`;
    const ul = document.createElement('ul');
    if (data.subdirectories && typeof data.subdirectories === 'object') {
        for (const [dirName, subDirData] of Object.entries(data.subdirectories)) {
            const li = document.createElement('li');
            li.textContent = dirName;
            li.classList.add('folder');
            li.addEventListener('click', () => {
                historyStack.push(currentData);
                updateTree(subDirData, parentElement, currentDirElement, backButton);
            });
            ul.appendChild(li);
        }
    }
    if (Array.isArray(data.files)) {
        data.files.forEach((file) => {
            if (file && file.path) {
                const li = document.createElement('li');
                li.textContent = file.path;
                li.classList.add('file');
                const extension = file.path.split('.').pop();
                if (extension !== file.path) {
                    li.setAttribute('data-extension', `.${extension}`);
                }
                ul.appendChild(li);
            }
        });
    }
    else if (data.files === "") {
        console.log('No files in this directory');
    }
    else {
        console.error('Expected "files" to be an array or an empty string');
    }
    parentElement.appendChild(ul);
}
document.addEventListener('DOMContentLoaded', () => {
    const popup = document.getElementById('popup');
    const openPopupBtn = document.getElementById('viewTreeBtn');
    const closePopupBtn = document.getElementById('closePopupBtn');
    const popupFileTree = document.getElementById('popupFileTree');
    backButton = document.getElementById('backButton');
    currentDirElement = document.getElementById('currentDir');
    const setInitialDataAndDisplay = (popupFileTree, currentDirElement, backButtonElement) => {
        fetch('filemap.json')
            .then(response => response.json())
            .then(data => {
            const topLevelData = {
                path: data.path,
                files: data.files === "" || Array.isArray(data.files) ? data.files : [],
                subdirectories: data.subdirectories === "" || typeof data.subdirectories === 'object' ? data.subdirectories : {}
            };
            currentDirElement.textContent = topLevelData.path;
            initialData = topLevelData;
            updateTree(initialData, popupFileTree, currentDirElement, backButtonElement);
        })
            .catch(error => console.error('Error loading JSON:', error));
    };
    openPopupBtn.addEventListener('click', () => {
        historyStack = [];
        historyStack.push(initialData.path);
        setInitialDataAndDisplay(popupFileTree, currentDirElement, backButton);
        popup.style.display = 'block';
    });
    closePopupBtn.addEventListener('click', () => {
        popup.style.display = 'none';
    });
    window.addEventListener('click', (event) => {
        if (event.target === popup) {
            popup.style.display = 'none';
        }
    });
    backButton.addEventListener('click', () => {
        if (historyStack.length > 1) {
            const previousData = historyStack.pop();
            updateTree(previousData, popupFileTree, currentDirElement, backButton);
        }
    });
    if (document.getElementById('mainFileTree')) {
        container = document.getElementById('mainFileTree');
        setInitialDataAndDisplay(container, currentDirElement, backButton);
    }
    else {
        container = popupFileTree;
        setInitialDataAndDisplay(container, currentDirElement, backButton);
    }
});
window.handleUpload = handleUpload;
