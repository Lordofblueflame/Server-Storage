let container: HTMLElement | null = null;
let currentDirElement: HTMLElement | null = null;
let backButton: HTMLButtonElement | null = null;
let currentData: any = null;
let historyStack: any[] = [];
let initialData: any = null;

function updateTree(
    data: any,
    parentElement: HTMLElement,
    currentDirElement: HTMLElement,
    backButtonElement: HTMLButtonElement
): void {
    if (!data || typeof data !== 'object') {
        return;
    }

    currentData = data;
    displayTree(currentData, parentElement, currentDirElement);

    if (backButtonElement) {
        if (historyStack.length > 0) {
            backButtonElement.style.display = 'inline-block';
            backButtonElement.classList.add('active');
        } else {
            backButtonElement.style.display = 'none';
            backButtonElement.classList.remove('active');
        }
    } else {
        console.error('backButtonElement is null');
    }
}

function handleUpload(): void {
    const fileInput = document.getElementById('file-input') as HTMLInputElement;
    fileInput.click();
}

function displayTree(data: any, parentElement: HTMLElement, currentDirElement: HTMLElement): void {
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
                updateTree(subDirData, parentElement, currentDirElement, backButton!);
            });

            ul.appendChild(li);
        }
    }

    if (Array.isArray(data.files)) {
        data.files.forEach((file: { path: string | null; }) => {
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
    const popup = document.getElementById('popup') as HTMLElement;
    const openPopupBtn = document.getElementById('viewTreeBtn') as HTMLButtonElement;
    const closePopupBtn = document.getElementById('closePopupBtn') as HTMLSpanElement;
    const popupFileTree = document.getElementById('popupFileTree') as HTMLElement;

    backButton = document.getElementById('backButton') as HTMLButtonElement;
    currentDirElement = document.getElementById('currentDir') as HTMLElement;

    const setInitialDataAndDisplay = (
        popupFileTree: HTMLElement,
        currentDirElement: HTMLElement,
        backButtonElement: HTMLButtonElement
    ) => {
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
        setInitialDataAndDisplay(popupFileTree, currentDirElement!, backButton!);
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
        if (historyStack.length > 0) {
            const previousData = historyStack.pop()!;
            updateTree(previousData, popupFileTree, currentDirElement!, backButton!);
        }
    });

    if (document.getElementById('mainFileTree')) {
        container = document.getElementById('mainFileTree') as HTMLElement;
        setInitialDataAndDisplay(container, currentDirElement!, backButton!);
    } else {
        container = popupFileTree;
        setInitialDataAndDisplay(container, currentDirElement!, backButton!);
    }
});

(window as any).handleUpload = handleUpload;
