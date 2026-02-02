import { Component, Input } from '@angular/core';

@Component({
  selector: 'app-file-view',
  standalone: true,
  imports: [],
  templateUrl: './file-view.component.html',
  styleUrls: ['./file-view.component.css']
})
export class FileViewComponent {
  @Input() path: string = '';
  @Input() size: number = 0;  
  @Input() last_write_time: string = '';
  
  getNameFromPath(path: string): string {
    return path.split('\\').pop() || '';
  }

  formatSize(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    else if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(2)} KB`;
    else if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    else return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  }
}
