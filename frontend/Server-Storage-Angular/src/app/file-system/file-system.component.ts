import { CommonModule } from '@angular/common';
import { Component, OnInit } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { HuginApiService, RealtimeEntry } from '../services/hugin-api.service';

@Component({
  selector: 'app-file-system',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './file-system.component.html',
  styleUrls: ['./file-system.component.css'],
})
export class FileSystemComponent implements OnInit {
  token = '';
  health = 'unknown';
  hostId = '';
  rootPath = '';
  currentDirectory = '';
  entries: RealtimeEntry[] = [];
  selectedFile: RealtimeEntry | null = null;
  uploadFileName = `note-${Date.now()}.txt`;
  uploadContent = 'Hello from Server-Storage MVP demo.\n';
  statusMessage = '';
  errorMessage = '';
  isBusy = false;

  constructor(private readonly huginApi: HuginApiService) {}

  ngOnInit(): void {
    this.token = localStorage.getItem('huginLocalToken') ?? '';
    void this.checkHealth();
    if (this.token) {
      void this.connect();
    }
  }

  async checkHealth(): Promise<void> {
    try {
      const health = await this.huginApi.health();
      this.health = health.ok ? 'healthy' : 'degraded';
    } catch (error) {
      this.health = 'offline';
      this.errorMessage = this.describeError(error);
    }
  }

  async connect(): Promise<void> {
    if (!this.token.trim()) {
      this.errorMessage = 'Paste the local access token first.';
      return;
    }

    this.isBusy = true;
    this.errorMessage = '';
    this.statusMessage = 'Loading latest materialized snapshot...';
    localStorage.setItem('huginLocalToken', this.token.trim());

    try {
      const snapshot = await this.huginApi.snapshot(this.token.trim());
      this.hostId = snapshot.host_id;
      this.rootPath = snapshot.path;
      await this.loadDirectory(snapshot.path);
      this.statusMessage = `Connected to ${snapshot.host_id}. Snapshot ${snapshot.snapshot_id}, ${snapshot.entry_count} entries.`;
    } catch (error) {
      this.errorMessage = this.describeError(error);
      this.statusMessage = '';
    } finally {
      this.isBusy = false;
    }
  }

  async loadDirectory(path: string): Promise<void> {
    if (!this.hostId) {
      await this.connect();
      return;
    }

    this.isBusy = true;
    this.errorMessage = '';
    this.selectedFile = null;

    try {
      const tree = await this.huginApi.tree(this.token.trim(), this.hostId, path);
      this.currentDirectory = tree.path;
      this.entries = [...tree.entries].sort((left, right) => {
        if (left.type !== right.type) {
          return left.type === 'directory' ? -1 : 1;
        }
        return left.name.localeCompare(right.name);
      });
      this.statusMessage = `Showing ${this.entries.length} item(s) from snapshot ${tree.snapshot_id}.`;
    } catch (error) {
      this.errorMessage = this.describeError(error);
      this.statusMessage = '';
    } finally {
      this.isBusy = false;
    }
  }

  async goParent(): Promise<void> {
    if (!this.currentDirectory || this.currentDirectory === this.rootPath) {
      return;
    }
    const parent = this.currentDirectory.replace(/\/+$/, '').replace(/\/[^/]*$/, '') || '/';
    await this.loadDirectory(parent);
  }

  selectEntry(entry: RealtimeEntry): void {
    if (entry.type === 'directory') {
      void this.loadDirectory(entry.path);
      return;
    }
    this.selectedFile = entry;
  }

  async uploadTextFile(): Promise<void> {
    if (!this.currentDirectory) {
      this.errorMessage = 'Open a directory before uploading.';
      return;
    }
    if (!this.uploadFileName.trim() || this.uploadFileName.includes('/') || this.uploadFileName.includes('\\')) {
      this.errorMessage = 'File name must be a single file name.';
      return;
    }

    this.isBusy = true;
    this.errorMessage = '';
    this.statusMessage = 'Uploading file...';

    try {
      const upload = await this.huginApi.uploadText(
        this.token.trim(),
        this.currentDirectory,
        this.uploadFileName.trim(),
        this.uploadContent,
      );
      this.statusMessage = `Uploaded ${upload.bytes_written} byte(s) to ${upload.path}. Waiting for watch refresh may take a moment.`;
      await this.loadDirectory(this.currentDirectory);
    } catch (error) {
      this.errorMessage = this.describeError(error);
      this.statusMessage = '';
    } finally {
      this.isBusy = false;
    }
  }

  async downloadSelected(): Promise<void> {
    if (!this.selectedFile) {
      return;
    }

    this.isBusy = true;
    this.errorMessage = '';

    try {
      const download = await this.huginApi.download(this.token.trim(), this.selectedFile.path);
      const bytes = Uint8Array.from(atob(download.content_b64), (char) => char.charCodeAt(0));
      const blob = new Blob([bytes]);
      const link = document.createElement('a');
      link.href = URL.createObjectURL(blob);
      link.download = this.selectedFile.name || 'download.bin';
      link.click();
      URL.revokeObjectURL(link.href);
      this.statusMessage = `Downloaded ${download.size_bytes} byte(s) from ${download.path}.`;
    } catch (error) {
      this.errorMessage = this.describeError(error);
    } finally {
      this.isBusy = false;
    }
  }

  formatSize(bytes: number): string {
    if (bytes < 1024) {
      return `${bytes} B`;
    }
    if (bytes < 1024 * 1024) {
      return `${(bytes / 1024).toFixed(1)} KB`;
    }
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  private describeError(error: unknown): string {
    if (typeof error === 'object' && error !== null && 'error' in error) {
      const maybeHttpError = error as { error?: { message?: string }; message?: string; status?: number };
      if (maybeHttpError.error?.message) {
        return maybeHttpError.error.message;
      }
      if (maybeHttpError.message) {
        return maybeHttpError.message;
      }
      if (maybeHttpError.status) {
        return `Request failed with HTTP ${maybeHttpError.status}.`;
      }
    }
    return error instanceof Error ? error.message : 'Request failed.';
  }
}
