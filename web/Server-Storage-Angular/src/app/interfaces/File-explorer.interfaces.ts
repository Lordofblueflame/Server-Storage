export interface File {
    path: string;
    last_write_time: string;
    file_size: number;
}

export interface Dictionary {
    path: string;
    subdirectories: { [key: string]: Dictionary } | ""; 
    files: File[] | "";
}