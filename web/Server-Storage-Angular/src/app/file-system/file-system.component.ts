import { Component, NgModule, OnInit } from '@angular/core';
import { JsonLoaderService } from '../services/json-loader.service';
import { CommonModule } from '@angular/common';
import { DisplayFileSystemComponent } from '../display-file-system/display-file-system.component';
import { HttpClientModule } from '@angular/common/http';

@Component({
  selector: 'app-file-system',
  standalone: true,
  imports: [CommonModule, 
    DisplayFileSystemComponent,
    HttpClientModule
  ],
  templateUrl: './file-system.component.html',
  styleUrls: ['./file-system.component.css'],
  providers: [JsonLoaderService],
})

export class FileSystemComponent implements OnInit {
  currentDirectory: string = "C:\\";
  currentDirectories: Array<{ path: string }> = [];
  currentFiles: Array<{ path: string, last_write_time: string, file_size: number }> = [];
  errorMessage: string | null = null;

  constructor(private dataLoaderService: JsonLoaderService) {}

  ngOnInit(): void {
    console.log('FileSystemComponent initialized');
    this.loadDirectory(this.currentDirectory);
  }

  public async loadDirectory(path: string): Promise<void> {
    console.log('Loading directory with path: ${path}');
    this.errorMessage = null;

    try {
      const directoryData = await this.dataLoaderService.loadDirectory(path);
      if (directoryData) {
        this.currentDirectory = directoryData.currentDirectory;
        this.currentDirectories = directoryData.currentDirectories;
        this.currentFiles = directoryData.currentFiles;
        console.log('Directory loaded successfully', this.currentDirectory, this.currentDirectories, this.currentFiles);
      } else {
        this.currentDirectories = [];
        this.currentFiles = [];
        this.errorMessage = 'Directory not found: ${path}';
      }
    } catch (error) {
      console.error('Error loading directory:', error);
      this.errorMessage = 'Failed to load directory data. Please try again later.';
      this.currentDirectories = [];
      this.currentFiles = [];
    }
  }

  async navigateToSubdirectory(subdirectoryPath: string): Promise<void> {
    console.log('Navigating to subdirectory with path:', subdirectoryPath);
    try {
      await this.loadDirectory(subdirectoryPath);
    } catch (error) {
      console.error('Error during navigation:', error);
    }
  }
  
}