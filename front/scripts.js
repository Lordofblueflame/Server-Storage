let container = null;
let currentDirElement = null;
let backButton = null;
let currentData = null;
let historyStack = []; // Stack to manage history
let initialData = null; // Store the initial directory data

function updateTree(data) {
    if (!data || typeof data !== 'object') {
        return;
    }

    currentData = data;
    displayTree(currentData, container, currentDirElement);

    // Update the back button state
    if (historyStack.length > 0) {
        backButton.style.display = 'inline-block';
        backButton.classList.add('active');
    } else {
        backButton.style.display = 'none';
        backButton.classList.remove('active');
    }
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
                historyStack.push(currentData); // Save the current directory to history
                updateTree(subDirData); // Navigate into the subdirectory
            });

            ul.appendChild(li);
        }
    }

    if (Array.isArray(data.files)) {
        data.files.forEach(file => {
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
    } else if (data.files === "") {
        console.log('No files in this directory');
    } else {
        console.error('Expected "files" to be an array or an empty string');
    }

    parentElement.appendChild(ul);
}

document.addEventListener('DOMContentLoaded', () => {
    container = document.getElementById('fileTree');
    currentDirElement = document.getElementById('currentDir');
    backButton = document.getElementById('backButton');

    if (!container || !currentDirElement || !backButton) {
        return;
    }

    backButton.addEventListener('click', () => {
        if (historyStack.length > 0) {
            const previousData = historyStack.pop(); // Get the last entry from history
            updateTree(previousData); // Go back to the previous directory
        }
    });

    fetch('filemap.json')
        .then(response => response.json())
        .then(data => {
            const topLevelData = {
                path: data.path,
                files: data.files === "" || Array.isArray(data.files) ? data.files : [],
                subdirectories: data.subdirectories === "" || typeof data.subdirectories === 'object' ? data.subdirectories : {}
            };
            initialData = topLevelData; // Set the initial directory data
            updateTree(initialData);
        })
        .catch(error => console.error('Error loading JSON:', error));
});
