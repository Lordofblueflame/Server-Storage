import { Component, EventEmitter, Input, Output } from '@angular/core';

@Component({
  selector: 'app-directory-view',
  standalone: true,
  imports: [],
  templateUrl: './directory-view.component.html',
  styleUrls: ['./directory-view.component.css']
})
export class DirectoryViewComponent {
  @Input() path: string = '';
  @Output() callback = new EventEmitter<string>();

  handleClick(event: Event): void {
    event.preventDefault();
    console.log('Emitting path:', this.path);
    this.callback.emit(this.path);
  }
  

  getNameFromPath(path: string): string {
    return path.split('\\').pop() || '';
  }
}
