import { Component } from '@angular/core';
import { RouterOutlet } from '@angular/router';
import { FileComponent } from '../file/file.component';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet,FileComponent],
  templateUrl: './app.component.html',
  styleUrl: './app.component.css'
})

export class AppComponent {
  title = 'Server-Storage-Angular';
}
