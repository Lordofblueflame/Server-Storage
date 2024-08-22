import { HttpClient } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { firstValueFrom } from 'rxjs';
import { Dictionary } from '../interfaces/File-explorer.interface';

@Injectable({
  providedIn: 'root',
})
export class JsonLoaderService {
  private dataUrl = 'assets/filemap.json';

  constructor(private http: HttpClient) {}

  async loadDirectory(path: string): Promise<{
    currentDirectory: string;
    currentDirectories: Array<{ path: string }>;
    currentFiles: Array<{ path: string, last_write_time: string, file_size: number }>;
    } | null> {
    console.log(`Loading directory for path: ${path}`);
    try {
      const directory = await firstValueFrom(this.http.get<Dictionary>(this.dataUrl));
      console.log(`Loaded directory object:`, directory);
  
      const foundDirectory = this.findDirectoryByPath(directory, path);
      console.log(`Found directory for path "${path}":`, foundDirectory);
  
      if (foundDirectory) {
        const currentDirectories = foundDirectory.subdirectories && typeof foundDirectory.subdirectories === 'object'
          ? Object.values(foundDirectory.subdirectories).map((sub: any) => ({ path: sub.path }))
          : [];
  
        const currentFiles = Array.isArray(foundDirectory.files)
          ? foundDirectory.files.map(file => ({
              path: file.path,
              last_write_time: file.last_write_time,
              file_size: file.file_size
            }))
          : [];
  
        return {
          currentDirectory: foundDirectory.path,
          currentDirectories,
          currentFiles
        };
      } else {
        console.warn(`Directory not found: ${path}`);
        return null;
      }
    } catch (error) {
      console.error('Error loading directory:', error);
      return null;
    }
  }
  private findDirectoryByPath(directory: Dictionary, path: string): Dictionary | null {
    console.log(`Searching for path "${path}" in directory:`, directory);

    if (directory.path === path) {
      console.log(`Match found: ${directory.path}`);
      return directory;
    }

    if (directory.subdirectories && typeof directory.subdirectories === 'object') {
      for (let key in directory.subdirectories) {
        if (directory.subdirectories.hasOwnProperty(key)) {
          const subdirectory = directory.subdirectories[key];
          const found = this.findDirectoryByPath(subdirectory, path);
          if (found) {
            return found;
          }
        }
      }
    }

    console.warn(`No match found for path: ${path}`);
    return null;
  }
}