import { Component } from '@angular/core';
import { RouterOutlet } from '@angular/router';
import { FileSystemComponent } from './file-system/file-system.component';
@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet, FileSystemComponent],
  templateUrl: './app.component.html',
  styleUrl: './app.component.css'
})

export class AppComponent {
  title = 'Server-Storage-Angular';
}
