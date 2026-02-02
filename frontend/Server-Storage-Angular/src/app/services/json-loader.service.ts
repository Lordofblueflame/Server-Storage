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
          ? Object.values(foundDirectory.subdirectories).map((sub: Dictionary) => ({ path: sub.path }))
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

  findDirectoryByPath(directory: Dictionary, path: string): Dictionary | null {
    console.log(`Searching for path "${path}" in directory:`, directory);
  
    const pathSegments = path.split('\\').filter(Boolean);
    console.log(`Path segments:`, pathSegments);
    
    const targetDepth = pathSegments.length;
    console.log(`Target depth: ${targetDepth}`);
  
    let currentDir: Dictionary | null = directory;
    let currentDepth = 0;
  
    if (currentDir.path === 'C:\\') {
      console.log(`Root directory matched: ${currentDir.path}`);
      currentDepth++;
    } else {
      console.warn(`Root directory not matched, expected "C:\\", found: ${currentDir.path}`);
      return null;
    }
  
    while (currentDir && currentDepth < targetDepth) {
      const segment = pathSegments[currentDepth];
      console.log(`At depth ${currentDepth}, searching for segment: "${segment}"`);
  
      const subdirectories = currentDir.subdirectories as Record<string, Dictionary>;
      
      if (subdirectories) {
        const subdirectoryKey = Object.keys(subdirectories).find(key => {
          return subdirectories[key].path.endsWith(segment);
        });
  
        if (subdirectoryKey) {
          currentDir = subdirectories[subdirectoryKey];
          console.log(`Matched subdirectory: "${subdirectoryKey}" at depth ${currentDepth}, moving deeper.`);
          currentDepth++;
        } else {
          console.warn(`No match found for segment: "${segment}" at depth ${currentDepth}`);
          return null;
        }
      } else {
        console.warn(`No subdirectories to search at depth ${currentDepth}`);
        return null;
      }
    }
  
    if (currentDir && currentDir.path === path) {
      console.log(`Successfully found the directory for path: "${path}"`);
      return currentDir;
    } else {
      console.warn(`Final directory path does not match. Expected "${path}", found "${currentDir?.path}"`);
      return null;
    }
  }
}