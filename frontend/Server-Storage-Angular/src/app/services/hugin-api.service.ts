import { HttpClient, HttpHeaders } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { firstValueFrom } from 'rxjs';

export interface HealthResponse {
  ok: boolean;
  [key: string]: unknown;
}

export interface RealtimeSnapshotResponse {
  ok: boolean;
  host_id: string;
  snapshot_id: number;
  last_sequence: number;
  entry_count: number;
  path: string;
}

export interface RealtimeEntry {
  id: string;
  parent_id: string;
  path: string;
  name: string;
  type: 'file' | 'directory' | string;
  size_bytes: number;
  created_at: string;
  modified_at: string;
  accessed_at: string;
  permissions: string;
}

export interface RealtimeTreeResponse {
  ok: boolean;
  host_id: string;
  path: string;
  snapshot_id: number;
  entries: RealtimeEntry[];
}

export interface UploadResponse {
  ok: boolean;
  path: string;
  bytes_written: number;
}

export interface DownloadResponse {
  ok: boolean;
  path: string;
  size_bytes: number;
  content_b64: string;
}

@Injectable({
  providedIn: 'root',
})
export class HuginApiService {
  private readonly apiBase = '/api/v1';

  constructor(private readonly http: HttpClient) {}

  health(): Promise<HealthResponse> {
    return firstValueFrom(this.http.get<HealthResponse>(`${this.apiBase}/health`));
  }

  snapshot(token: string): Promise<RealtimeSnapshotResponse> {
    return firstValueFrom(
      this.http.post<RealtimeSnapshotResponse>(
        `${this.apiBase}/realtime/snapshot`,
        {},
        { headers: this.authHeaders(token) },
      ),
    );
  }

  tree(token: string, hostId: string, path: string): Promise<RealtimeTreeResponse> {
    return firstValueFrom(
      this.http.post<RealtimeTreeResponse>(
        `${this.apiBase}/realtime/tree`,
        { host_id: hostId, path },
        { headers: this.authHeaders(token) },
      ),
    );
  }

  uploadText(token: string, destinationDir: string, fileName: string, content: string): Promise<UploadResponse> {
    return firstValueFrom(
      this.http.post<UploadResponse>(
        `${this.apiBase}/files/upload`,
        {
          destination_dir: destinationDir,
          file_name: fileName,
          content_b64: this.toBase64(content),
        },
        { headers: this.authHeaders(token) },
      ),
    );
  }

  download(token: string, path: string): Promise<DownloadResponse> {
    return firstValueFrom(
      this.http.post<DownloadResponse>(
        `${this.apiBase}/files/download`,
        { path },
        { headers: this.authHeaders(token) },
      ),
    );
  }

  private authHeaders(token: string): HttpHeaders {
    return new HttpHeaders({
      Authorization: `Bearer ${token}`,
      Accept: 'application/json',
    });
  }

  private toBase64(value: string): string {
    const bytes = new TextEncoder().encode(value);
    let binary = '';
    bytes.forEach((byte) => {
      binary += String.fromCharCode(byte);
    });
    return btoa(binary);
  }
}
